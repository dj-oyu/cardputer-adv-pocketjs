#include "app_session.h"
#include "board.h"
#include "fonts.h"
#include "pocketjs/guest.h"
#include "pocketjs/guest_quickjs.h"
#include "pocketjs/ui_core.h"
#include "pocketjs/ui_qjs.h"
#include "pocketjs/render_rgb565.h"
#include "jsconsole.h"
#include "jsfont.h"
#include "pocket_api.h"
#include "pocket_random.h"
#include "pocket_storage.h"
#include "pocket_fs.h"
#include "pocket_imu.h"
#include "pocket_av.h"
#include "pocket_capture.h"
#include "pocket_io.h"
#include "pocket_net.h"
#include "pocket_ble.h"
#include "pocket_ui.h"
#include "pocket_text.h"
#include "pocket_app.h"
#include "pocket_bridge.h"
#include "pocket_workspace.h"
#include "pocket_overlay.h"
#include "app_registry.h"
#include "pet_assets.h"
#include "pet_hub.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "vmprobe.h"
#include "vm_wake.h"
#include <stdatomic.h>
#include <string.h>

extern const char hello_start[] asm("_binary_main_js_start");
extern const char hello_end[] asm("_binary_main_js_end");
// TEMPORARY: diagnostic 7 proves the legacy node guard fires.
extern const char nodecap_start[] asm("_binary_nodecap_js_start");
extern const char pet_start[] asm("_binary_pet_js_start");
static pocketjs_guest_t *guest;
static pocketjs_ui_core_t *core;
static pocketjs_ui_qjs_t *binding;
static pocketjs_rgb565_renderer_t *renderer;
static pocketjs_rgb565_target_t *target;
static atomic_bool stop_requested;
static int64_t deadline;
// L1 (docs/vm-L1-design.md). Armed once per turn and handed to the guest, so
// frame()'s drain and the next turn's continuation drain measure against the
// same turn start.
static vm_budget_t budget;
// Presses that arrived on a turn spent finishing the previous turn's queue.
// pocket_ui_pump() is a delivery into JavaScript and so is held back with the
// rest; the mask is OR'd into the first turn that runs the pumps, which is
// what keeps a keystroke from being dropped instead of merely delayed.
static uint32_t deferred_buttons;
// Consecutive turns that ended with the queue still non-empty (sec.5.2).
static unsigned continuation_turns;
// What the LAST app_tick() was, read by main.c to decide what to wait for.
static bool turn_continued;
// When present_frame() last reached the panel. Only the continuation path
// reads it; see there for why the display, unlike the turn, is still paced.
static int64_t last_present_us;
static unsigned frames;
static bool redraw;
static double render_sum, present_sum, kernel_sum, turn_sum;
static unsigned painted, ticks;
// Hand-written PIE kernels for the two ops this renderer actually asks for
// (opaque fill, coverage-mask blend); anything they cannot honour exactly is
// declined and the Rust software path draws it.
extern const pocketjs_rgb565_accelerator_t render_accel;
extern uint32_t render_accel_cycles;
// Borrowed for the length of a start; the Playground owns the bytes and does
// not edit them while a run is up.
static const char *user_source;
static size_t user_length;
// Evaluated first, in the same realm, when the caller has one. The tutorial's
// chapters use it for the eight lines that build the text node they work on.
static const char *user_prelude;
static size_t user_prelude_length;
// THE ONE PLACE THE GUEST'S LIFETIME CHANGES.
//
// Until now a session was bounded by entering and leaving an app screen: the
// home screen's loop built a guest when somebody pressed Enter on a row and
// destroyed it when they pressed Back. docs/common-api.md 3.1 adds a second
// bound -- the HOME SCREEN owns a session for as long as it is on show -- and
// that is the whole of the difference. Everything else about the contract is
// unchanged and deliberately so: app_stop() below still tears the surfaces
// down in the reverse of the order they were built, and every module holding a
// guest callback is still reset before the guest is destroyed. An overlay
// session is a session; it is only started and ended by a different event.
//
// The flag is what an overlay session does NOT get: no Rust UI core, no font
// atlas, no rgb565 renderer, and a much smaller guest heap. See
// pocket_overlay.h for why drawing goes through a host display list instead.
static bool overlay_session;
void app_force_redraw(void) { redraw=true; }
static esp_err_t present_frame(pocketjs_ui_frame_view_t *frame);

// The session watchdog. Registered through the guest rather than with
// JS_SetInterruptHandler directly: QuickJS has ONE handler slot and three
// callers used to overwrite each other in it (sec.5.3), which is why the
// guest's own epoch handler had been dead for as long as this one existed.
static int interrupt(void *opaque) {
    (void)opaque;
    return atomic_load(&stop_requested) || esp_timer_get_time()>deadline;
}
void app_vm_watchdog(int (*fn)(void *), void *opaque) {
    if(guest) pocketjs_guest_set_watchdog(guest,fn?fn:interrupt,opaque);
}
void app_request_stop(void) { atomic_store(&stop_requested,true); }

// One clock read, at the top of every turn, shared by the 250 ms watchdog
// deadline and by the job budget: sec.1.2 measures the budget from TURN start,
// not from drain start, because the turn length is what a completion's latency
// actually is (measured (device), L0 sec.2.1).
//
// CONFIG_POCKET_VM_SCHED off makes this an unlimited budget, and an unlimited
// budget cannot yield -- so nothing below the guest's drain loop can tell the
// difference from the pre-L1 code. That is the revert switch.
static void arm_turn(uint32_t buttons) {
    const int64_t now=esp_timer_get_time();
    deadline=now+250000;
#ifdef CONFIG_POCKET_VM_SCHED
    // The Back turn (main.c calls app_tick(0x2000) once so the guest can save)
    // gets room to finish rather than be cut: the session ends immediately
    // after it, so no reordering it causes can be observed.
    if(buttons&0x2000)
        vm_budget_begin_full(&budget,VM_LEAVE_BUDGET_US,VM_JOB_STRIDE,
                             VM_JOB_FLOOR,VM_LEAVE_BACKSTOP);
    else
        vm_budget_begin(&budget,VM_TURN_BUDGET_US);
#else
    (void)buttons;
    vm_budget_begin(&budget,0);
#endif
    // vm_budget_begin does its own read through vm_clock rather than being
    // handed `now`: which clock the budget uses is vm_clock's decision (the
    // measurement says a cycle counter is 33x cheaper than the timer), and
    // handing it a value in esp_timer's units would silently break the day
    // that decision changes. One extra read a turn, against a turn measured
    // in milliseconds.
    pocketjs_guest_budget(guest,&budget);
}

// sec.5.2: is THIS logical drain a runaway? The question is what the drain has
// spent, not how many turns it took to spend it. A turn ends when the wall
// clock says 8 ms -- which on a contended machine is mostly somebody else's
// time -- or when the 64-job backstop says so, and neither number is a measure
// of the guest's appetite: counting turns killed an honest 2,500-job chain
// (tools/vmtest/corpus/budget_honest_long_chain.js) at 134 ms of JS, where the
// 250 ms guard it replaced would have let it finish.
//
// VM_RUNAWAY_US totals only the time inside vm_sched_drain(), so everything
// the old deadline also charged -- frame(), the pumps, the render, the
// transfer -- is now free. That makes this guard strictly more permissive than
// the one it replaces: nothing the pre-L1 firmware ran to completion can be
// ended by it. Time the drain spent preempted is still charged, because no
// host-side clock can tell that time from the guest's; the answer to that is
// the size of the allowance, not a finer unit.
static bool drain_runaway(void) {
    int64_t us=0; uint64_t jobs=0;
    pocketjs_guest_drain_total(guest,&us,&jobs);
    if(us<VM_RUNAWAY_US && jobs<VM_RUNAWAY_JOBS) return false;
    ESP_LOGE("app","RUNAWAY one drain spent %lld us over %llu jobs in %u turns",
             (long long)us,(unsigned long long)jobs,continuation_turns+1u);
    jsconsole_set_error("JOB QUEUE RUNAWAY");
    return true;
}

// app_registry.c takes this rather than calling pocket_api_supported() itself,
// so that the registry stays free of the API surface and can be tested on a
// host that has neither.
static bool capability_supported(const char *name, void *user) {
    (void)user;
    return pocket_api_supported(name);
}

// pocketjs_guest_eval dumps an exception to stderr and returns ESP_FAIL, so the
// message never reaches the caller. The Playground needs it, and the guest's
// context is valid for the length of a synchronous owner-task call, so the
// evaluation happens here and the exception is copied out before it is freed.
//
// Two more evaluations follow the user's source: a wrapper that hands a
// runtime exception to the host before rethrowing it, and a trivial expression
// whose only job is to make pocketjs_guest_eval re-read globalThis.frame —
// that read is the only place the guest latches the frame function, and it is
// also how a source without one reports ESP_ERR_NOT_FOUND.
static const char FRAME_WRAP[] =
    "(function(){var f=globalThis.frame;if(typeof f!=='function')return;"
    "globalThis.frame=function(){try{return f.apply(this,arguments);}"
    "catch(e){__pjs_error(String(e),e&&e.stack);throw e;}};})()";

// One evaluation, with its exception reported the way the Playground needs it.
// `filename` is what the learner is shown in the error, so the prelude and the
// lesson are told apart when the failure is in the part nobody typed.
static esp_err_t eval_reporting(const char *source, size_t length,
                                const char *filename) {
    JSContext *ctx=pocketjs_guest_quickjs_context(guest);
    if(!ctx) return ESP_ERR_INVALID_STATE;

    JSValue result=JS_Eval(ctx,source,length,filename,JS_EVAL_TYPE_GLOBAL);
    if(JS_IsException(result)) {
        JSValue exception=JS_GetException(ctx);
        char message[128]={0};
        const char *text=JS_ToCString(ctx,exception);
        if(text) { snprintf(message,sizeof(message),"%s",text); JS_FreeCString(ctx,text); }
        JSValue stack=JS_GetPropertyStr(ctx,exception,"stack");
        // A stack getter can itself throw. Clearing the new pending exception
        // here keeps whatever the caller evaluates next -- the lesson after a
        // prelude, the frame wrapper after the source -- from inheriting it.
        if(JS_IsException(stack)) JS_FreeValue(ctx,JS_GetException(ctx));
        else if(!JS_IsUndefined(stack)&&!JS_IsNull(stack)) {
            const char *s=JS_ToCString(ctx,stack);
            if(s) {
                const char *nl=strchr(s,'\n');
                int n=nl?(int)(nl-s):(int)strlen(s);
                size_t used=strlen(message);
                snprintf(message+used,sizeof(message)-used," %.*s",n,s);
                JS_FreeCString(ctx,s);
            }
        }
        JS_FreeValue(ctx,stack);
        JS_FreeValue(ctx,exception);
        JS_FreeValue(ctx,result);
        jsconsole_set_error(message[0]?message:"evaluation failed");
        ESP_LOGW("app","EVAL_ERROR %s",message);
        return ESP_FAIL;
    }
    JS_FreeValue(ctx,result);
    return ESP_OK;
}

// The lesson's prelude and the learner's own lines, as two evaluations in one
// realm rather than one concatenated script.
//
// QuickJS keeps the global lexical environment on the realm, so the prelude's
// top-level `const t` is visible to the second evaluation -- measured on the
// board, not assumed. What that buys is the error message: a learner who
// misspells `print` on their only line was told `user.js:9:1`, because the
// prelude is eight lines and both halves were one script, and chapter 2 is
// about reading error messages. It also deletes the 8,704 byte buffer the join
// needed.
//
// This is safe only because a run always builds a fresh guest -- app_stop()
// destroys it and app_start_test() creates another -- so the prelude is never
// evaluated twice into one realm. That case throws a SyntaxError for the
// duplicate lexical binding, and it would arrive on the learner's second
// Ctrl+R, naming a line they never wrote.
static esp_err_t eval_user_source(const char *source, size_t length) {
    JSContext *ctx=pocketjs_guest_quickjs_context(guest);
    if(!ctx) return ESP_ERR_INVALID_STATE;
    if(user_prelude) {
        esp_err_t pre=eval_reporting(user_prelude,user_prelude_length,"prelude.js");
        if(pre!=ESP_OK) return pre;
    }
    esp_err_t err=eval_reporting(source,length,"user.js");
    if(err!=ESP_OK) return err;

    JSValue wrap=JS_Eval(ctx,FRAME_WRAP,sizeof(FRAME_WRAP)-1,"wrap.js",JS_EVAL_TYPE_GLOBAL);
    if(JS_IsException(wrap)) JS_FreeValue(ctx,JS_GetException(ctx));
    JS_FreeValue(ctx,wrap);

    // ESP_ERR_NOT_FOUND here means the source defined no frame; that is the
    // caller's cue to run it as an expression rather than as an app.
    return pocketjs_guest_eval(guest,"0",1,"bind-frame.js");
}
// The guest's stats, taken while the guest still exists. app_stop() destroys
// it and NULLs the pointer BEFORE calling app_report(), so the live read below
// answers zero at the one call site the warning is written for: sec.3.2's
// "jobs dropped at stop" could never fire. Latched at the only moment the
// answer is both known and final -- after the stop hook has had its 200 ms of
// JS_ExecutePendingJob, before JS_FreeRuntime discards whatever is left.
static pocketjs_guest_stats_t final_stats;
void app_report(void) {
    pocketjs_guest_stats_t stats={.struct_size=sizeof(stats)};
    if(guest) pocketjs_guest_stats(guest,&stats);
    // sec.3.2: a session can end with work still queued, and that work is
    // discarded unrun -- JS_FreeRuntime's own behaviour, and the same choice
    // pocket_api_reset() already makes for in-flight promises. Said out loud
    // because an app whose last Promise never settled would otherwise have no
    // trace of why. A new line; no contracted marker's format is touched.
    const pocketjs_guest_stats_t *ended=guest?&stats:&final_stats;
    if(ended->jobs_dropped)
        ESP_LOGW("app","jobs dropped at stop: queue was not empty (yields=%u continuations=%u)",
                 (unsigned)ended->yields,(unsigned)ended->continuations);
    // MEM stays on the LIVE stats (zeroes once the guest is gone, as it always
    // has): tools/memlog.py parses js= out of this line and compares the start
    // and stop pair, so changing what stop reports would change its budget.
    ESP_LOGI("app","MEM free=%u largest=%u js=%u frames=%u",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
        (unsigned)stats.heap_used,frames);
}
void app_stop(void) {
#ifdef CONFIG_POCKET_VM_PROBE
    vmprobe_session_reset();
#endif
    jsfont_detach();
    // Before the guest goes: the watches hold callbacks belonging to it, and a
    // promise still in flight holds its resolvers.
    // First: section 5 runs the stop hook before I/O cancellation and before
    // the subscriptions it may still want to use are taken away.
    pocket_app_reset();
    pet_assets_reset();
    pocket_imu_reset();
    pocket_av_reset();
    // Before pocket_api_reset(): a recorder holds the I2S RX channel and the
    // codec's ADC, and a read still waiting holds a promise slot.
    pocket_capture_reset();
    pocket_io_reset();
    // Before pocket_api_reset(): dropping the lease is what asks the radio to
    // come down, and a request still in flight has to be told to stop before
    // its promise slot is taken away.
    pocket_net_reset();
    pocket_fs_reset();
    // Before pocket_api_reset(): a picker still on screen holds a promise slot,
    // and giving the screen back is what posts its completion.
    pocket_workspace_reset();
    pocket_ui_reset();
    pocket_overlay_reset();
    // Before pocket_api_reset(): an open field holds three guest callbacks, and
    // a screen change closes the session -- which is what the end of a run is.
    pocket_text_reset();
    pocket_bridge_reset();
    pocket_api_reset();
    if(renderer && target) pocketjs_rgb565_abort(renderer,target);
    if(target) pocketjs_rgb565_target_destroy(target);
    if(renderer) pocketjs_rgb565_renderer_destroy(renderer);
    // Read before the runtime goes: app_report() below runs with guest == NULL,
    // so this is the last point at which "was anything still queued" has an
    // answer. pocket_app_reset() above has already given the stop hook its
    // 200 ms, so what is pending here is what really gets discarded.
    if(guest) {
        final_stats=(pocketjs_guest_stats_t){.struct_size=sizeof(final_stats)};
        pocketjs_guest_stats(guest,&final_stats);
    }
    if(guest) pocketjs_guest_destroy(guest);
    if(binding) pocketjs_ui_qjs_destroy(binding);
    if(core) pocketjs_ui_core_destroy(core);
    target=NULL;renderer=NULL;guest=NULL;binding=NULL;core=NULL;
    app_report();
    ESP_LOGI("app","APP_STOPPED");
}
esp_err_t app_start_test(char test) {
    esp_err_t err;
    // The USB diagnostics call this directly, so they are the one caller that
    // has not set these three. Left over from the boot overlay, overlay_session
    // skipped the test's source and its renderer, and the session died on its
    // first tick with no error line -- every diagnostic run after boot did.
    if(test) { user_source=NULL; user_prelude=NULL; overlay_session=false; }
    atomic_store(&stop_requested,false); frames=0;
    deferred_buttons=0; continuation_turns=0; turn_continued=false;
    last_present_us=0;
    // Cleared with them: a start that fails before a guest exists reaches
    // app_stop() with guest already NULL, and a stale latch would then blame
    // this session for the previous one's queue.
    final_stats=(pocketjs_guest_stats_t){.struct_size=sizeof(final_stats)};
    deadline=esp_timer_get_time()+2000000;
    pocketjs_guest_config_t gc;
    pocketjs_guest_config_defaults(&gc);
    // 160 KiB, not 128. The cap was chosen when the native API surface was
    // nothing; it has since grown and apps started failing to evaluate at all
    // while the system still had 59 KiB free during a run -- the cap was the
    // binding constraint, not the memory. Parsing peaks well above what the
    // program then retains, which is why a 6.5 KB source sat at 107 KiB and a
    // 6.7 KB one did not fit at 128. Lazy namespace installation bought the
    // 20 KiB that makes this size safe; without it the guest takes the room at
    // startup instead.
    //
    // It is also the reason an app cannot bring the radio up: with the guest
    // holding ~105 KB, esp_wifi_init is reached with 9 KB free and needs about
    // 48. See NET_RADIO_MIN_FREE in pocket_net.c. The fix is ordering, not a
    // smaller cap for everyone -- most apps never touch the radio.
    gc.heap_limit=160*1024; gc.stack_limit=20*1024; gc.prefer_psram=false;
    // An overlay runs WHILE a background scene is drawing, so it is sized for
    // what is left rather than for what a foreground app may take. 3.1 asks for
    // the check before the start rather than a failure during it; ui/overlay.c
    // makes the reservation and this is the cap it reserved against.
    // The stack limit stays at the foreground value. It was 8 KiB for one
    // build, on the reasoning that a small app needs a small stack -- but it
    // is a recursion depth bound for the parser, not memory that is saved by
    // being smaller, and lowering it only added a second unknown to a start
    // that was already failing.
    if(overlay_session) gc.heap_limit=OVERLAY_GUEST_HEAP;
#define TRY(expr) do {err=(expr);if(err!=ESP_OK)goto fail;}while(0)
    TRY(pocketjs_guest_create(&gc,&guest));
#ifdef CONFIG_POCKET_VM_PROBE
    vmprobe_static_report();
#endif
    pocketjs_guest_set_watchdog(guest,interrupt,NULL);
    // Replaces quickjs-libc's print, whose output only ever reaches stdout.
    jsconsole_clear();
    TRY(pocketjs_guest_quickjs_install_once(guest,"console",jsconsole_install,NULL));
    TRY(pocketjs_guest_quickjs_install_once(guest,"jsfont",jsfont_install,NULL));
    TRY(pocketjs_guest_quickjs_install_once(guest,"pocket",pocket_api_install,NULL));
    TRY(pocketjs_guest_quickjs_install_once(guest,"random",pocket_random_install,NULL));
    TRY(pocketjs_guest_quickjs_install_once(guest,"storage",pocket_storage_install,NULL));
    // 3.1: an overlay's default capability set is NARROWER than a foreground
    // app's, because it runs while nobody is looking at it. The narrowing is
    // done by not installing the surface at all, so capabilities.get() answers
    // supported=false for the radio, the microphone and the buses -- which is
    // the honest answer for this session, and costs the guest nothing.
    if(overlay_session) {
        TRY(pocketjs_guest_quickjs_install_once(guest,"app",pocket_app_install,NULL));
        TRY(pocketjs_guest_quickjs_install_once(guest,"overlay",pocket_overlay_install,NULL));
        // fs and av joined this list on 2026-09-09, and the reason is worth
        // stating because 3.1's narrowing is deliberate and this widens it.
        //
        // The narrowing exists because an overlay runs while nobody is looking
        // at it. That is no longer the whole truth: an overlay now REPLACES the
        // home screen's menu, so it runs while the person is looking directly
        // at it and operating it. The surfaces below are the ones a home screen
        // that plays music needs -- reading the card, and the player -- and
        // both are things the person started on purpose from a Settings row.
        //
        // What is still absent is the list that matters: no net, no ble, no
        // capture, no io, no bridge, no workspace. Nothing here can reach the
        // radio, the microphone or the buses, so capabilities.get() answers
        // supported=false for them, which is the honest answer for this session
        // and costs the guest nothing.
        TRY(pocketjs_guest_quickjs_install_once(guest,"fs",pocket_fs_install,NULL));
        TRY(pocketjs_guest_quickjs_install_once(guest,"av",pocket_av_install,NULL));
        goto surfaces_done;
    }
    TRY(pocketjs_guest_quickjs_install_once(guest,"fs",pocket_fs_install,NULL));
    TRY(pocketjs_guest_quickjs_install_once(guest,"imu",pocket_imu_install,NULL));
    TRY(pocketjs_guest_quickjs_install_once(guest,"av",pocket_av_install,NULL));
    // After "av": both contribute to pocket.audio, and the substrate runs
    // contributors in the order they registered.
    TRY(pocketjs_guest_quickjs_install_once(guest,"capture",pocket_capture_install,NULL));
    TRY(pocketjs_guest_quickjs_install_once(guest,"io",pocket_io_install,NULL));
    TRY(pocketjs_guest_quickjs_install_once(guest,"net",pocket_net_install,NULL));
    TRY(pocketjs_guest_quickjs_install_once(guest,"ble",pocket_ble_install,NULL));
    TRY(pocketjs_guest_quickjs_install_once(guest,"pui",pocket_ui_install,NULL));
    // After "pui": both contribute to pocket.input, and contributors run in
    // the order they registered.
    TRY(pocketjs_guest_quickjs_install_once(guest,"text",pocket_text_install,NULL));
    TRY(pocketjs_guest_quickjs_install_once(guest,"bridge",pocket_bridge_install,NULL));
    // After "console": it wraps print and console.log onto the section 7 ring.
    TRY(pocketjs_guest_quickjs_install_once(guest,"app",pocket_app_install,NULL));
    // After "app": launchContext and info join pocket.app, and a contributor
    // runs in the order it registered.
    TRY(pocketjs_guest_quickjs_install_once(guest,"workspace",pocket_workspace_install,NULL));
surfaces_done:
    if(overlay_session) goto source_ready;
    {
    pocketjs_ui_core_config_t cc;
    pocketjs_ui_core_config_defaults(&cc);
    cc.logical_width=LCD_W;cc.logical_height=LCD_H;cc.raster_density=1;cc.tick_hz=30;
    TRY(pocketjs_ui_core_create(&cc,&core));
    TRY(pocketjs_ui_core_load_font_atlas(core,font_small,sizeof(font_small)));
    TRY(pocketjs_ui_core_load_font_atlas(core,font_large,sizeof(font_large)));
    pocketjs_ui_qjs_config_t bc={.struct_size=sizeof(bc),.target_id="cardputer-adv",.host_abi=1};
    TRY(pocketjs_ui_qjs_create(guest,core,&bc,&binding));
    TRY(pocketjs_ui_qjs_mount(binding));
    // Japanese for JS nodes: slot 2 starts empty and grows as text is set, so
    // the wrapper has to be in place before any program runs.
    jsfont_attach(core);
    {
        JSContext *ctx=pocketjs_guest_quickjs_context(guest);
        if(ctx) {
            JSValue w=JS_Eval(ctx,JSFONT_WRAP,strlen(JSFONT_WRAP),
                              "jsfont.js",JS_EVAL_TYPE_GLOBAL);
            if(JS_IsException(w)) {
                JS_FreeValue(ctx,JS_GetException(ctx));
                ESP_LOGW("app","setText wrapper failed; Japanese will be tofu");
            }
            JS_FreeValue(ctx,w);
            // Same place, same reason: the binding exists now and no app source
            // has run. This one puts the node budget in front of the legacy
            // ui.createNode that every app in apps/ still uses.
            pocket_ui_attach(ctx);
        }
    }
    TRY(pocketjs_guest_quickjs_install_once(guest,"pet-hub",pet_hub_install,NULL));
    TRY(pocketjs_guest_quickjs_install_once(guest,"pet-assets",pet_assets_install,core));
    }
source_ready:;
    const char *source=user_source?user_source:hello_start;
    size_t length=user_source?user_length:(size_t)(hello_end-hello_start-1);
    // USB-only diagnostics exercise the same lifecycle and resource limits.
    switch(test) {
        case '1': source="(() => {"; break;
        case '2': source="while(true){}"; break;
        case '3': source="globalThis.frame=()=>{while(true){}}"; break;
        case '4': source="let a=[];while(true)a.push(new Uint8Array(4096))"; break;
        case '5': source="globalThis.frame=()=>{throw Error('test')}"; break;
        case '6': source="globalThis.frame=()=>{function f(){Promise.resolve().then(f)}f()}"; break;
    }
    if(test)length=strlen(source);
    // Section 3's registration check, and the last thing before the app's own
    // code runs: every surface has registered its capabilities by now, so an
    // app asking for one this build does not implement is refused here rather
    // than failing somewhere inside itself. The identity is applied at the same
    // moment, because the stores below are keyed by it and a session must not
    // inherit the last one's.
    {
        const app_manifest_t *manifest=app_registry_current();
        pocket_storage_set_owner(manifest->id);
        pocket_fs_set_owner(manifest->id);
        char reason[64];
        if(!app_registry_admit(manifest,POCKET_API_VERSION,capability_supported,
                               NULL,reason,sizeof reason)) {
            jsconsole_set_error(reason);
            ESP_LOGW("app","APP_REFUSED %s %s",manifest->id,reason);
            err=ESP_ERR_NOT_SUPPORTED;
            goto fail;
        }
        ESP_LOGI("app","APP_ID %s",manifest->id);
    }
    if(user_source) {
        TRY(eval_user_source(source,length));
    } else {
        TRY(pocketjs_guest_eval(guest,source,length,test?"diagnostic.js":"hello.js"));
    }
    if(!overlay_session) {
        pocketjs_rgb565_renderer_config_t rc;
        pocketjs_rgb565_renderer_config_defaults(&rc);rc.scale=1;
        TRY(pocketjs_rgb565_renderer_create(&rc,&renderer));
        TRY(pocketjs_rgb565_target_create(&target));
    }
    app_report();
    return ESP_OK;
fail:
    ESP_LOGE("app","START_FAILED %s",esp_err_to_name(err));
    app_stop();return err;
}
esp_err_t app_start(void) {
    user_source=NULL; user_prelude=NULL; overlay_session=false;
    return app_start_test(0);
}

esp_err_t app_start_overlay(const char *source, size_t length) {
    user_source=source; user_length=length;
    user_prelude=NULL; overlay_session=true;
    esp_err_t err=app_start_test(0);
    user_source=NULL;
    // Left standing on failure too: app_stop() has already run inside
    // app_start_test(), and the flag only ever decides what the NEXT start
    // builds. ui/overlay.c clears it by calling app_stop() itself.
    return err;
}

// One turn of an overlay. No damage plan, no strips, no bus: what an overlay
// draws is a display list the shell composites into its own frame, so the
// whole of the frame here is the guest's own JavaScript.
esp_err_t app_overlay_tick(void) {
    if(!guest) return ESP_ERR_INVALID_STATE;
    // The same runaway guard the foreground gets, and it was 50 ms until a
    // board run under the FLOWER scene threw "InternalError: interrupted"
    // inside a five-line loop that counts characters.
    //
    // THIS DEADLINE IS WALL CLOCK. It counts every microsecond the ui task
    // spends preempted -- by the decoder at priority 6, by the audio task at 7,
    // by a card read -- and none of that is the guest running. FLOWER draws at
    // 71 ms a frame with 53 ms of kernel, so the machine is contended enough
    // that a turn doing almost nothing can sit through 50 ms of somebody else's
    // work and be killed for it.
    //
    // The 50 ms was a cost control wearing a runaway guard's clothes. The cost
    // control already exists and is a different mechanism: budget_us with
    // over_limit consecutive turns, in ui/overlay.c, which measures the turn
    // and stops the overlay with a reason a person can read and undo. This one
    // throws inside whichever line the guest happened to be on, so an app gets
    // blamed for the system being busy. Two mechanisms, two jobs; this one is
    // "not coming back" and should be far beyond any honest turn.
    arm_turn(0);
    // The same continuation rule as app_tick() (sec.2.2), minus the surfaces an
    // overlay does not install. There is no UI core here, so the drain is
    // resumed directly instead of through the binding.
    if(pocketjs_guest_jobs_pending(guest)) {
        esp_err_t ce=pocketjs_guest_continue(guest);
        if(ce) return ce;
        if(pocketjs_guest_jobs_pending(guest)) {
#ifdef CONFIG_POCKET_VM_FAIR
            // Fair ordering, the overlay's share of it: the same rule and the
            // same reasons as app_tick() states at length, over the pumps an
            // overlay session actually installs. No exit() check and no
            // frame() here either.
            pocket_app_pump();
            pocket_overlay_pump();
            pocket_api_pump();
            pocket_fs_pump();
            pocket_av_pump();
#endif
            if(drain_runaway()) return ESP_ERR_TIMEOUT;
            continuation_turns++;
            // No display list this turn: the shell composites whatever the
            // overlay last produced, which is the same thing it does for a
            // turn the overlay chose not to draw in.
            return ESP_OK;
        }
    }
    continuation_turns=0;
    // Same place as app_tick(): only a turn that starts with an empty queue
    // may turn an exit() into a stop, because the stop is delivered as an
    // interrupt and would otherwise cut the drain it lands in.
    if(pocket_app_exit_requested()) app_request_stop();
    pocket_app_pump();
    // Before pocket_api_pump(), like every other producer: what these post is
    // settled by that call, and posting after it would delay every completion
    // by a frame. The order is app_tick()'s, minus the surfaces an overlay does
    // not install.
    pocket_overlay_pump();
    pocket_api_pump();
    // AFTER pocket_api_pump(), exactly as app_tick() has them: these two post
    // no completions of their own, they move bytes for work already promised.
    pocket_fs_pump();
    pocket_av_pump();
    pocketjs_guest_frame_t f={.struct_size=sizeof(f)};
    esp_err_t e=pocketjs_guest_frame(guest,&f);
    frames++;
    return e;
}

esp_err_t app_start_source(const char *prelude, size_t prelude_length,
                           const char *source, size_t length) {
    user_source=source; user_length=length;
    user_prelude=prelude; user_prelude_length=prelude_length;
    overlay_session=false;
    esp_err_t err=app_start_test(0);
    user_source=NULL; user_prelude=NULL;
    return err;
}

bool app_turn_continued(void) { return turn_continued; }

const char *app_error(void) {
    const char *e=jsconsole_error();
    return e?e:"";
}
// Every host surface's pump, in the one order the turn defines, and nothing
// else: no exit() check and no frame(). Extracted so that the fair-ordering
// continuation turn (CONFIG_POCKET_VM_FAIR, below) runs THE SAME LIST rather
// than a copy of it that drifts -- the order these are in is a set of
// decisions, each written next to its call, and two copies of it would mean
// two places to get those decisions wrong. Static and called from one place
// in the shipping build, so it costs nothing there.
static void run_pumps(uint32_t buttons) {
    // Watch deliveries before the frame, so a listener that updates a node and
    // the frame that draws it are the same turn rather than one apart.
    // First: it posts the sleeps that came due, so pocket_api_pump() settles
    // them this turn, and it is where Starting becomes Running.
    pocket_app_pump();
    // The keystroke that queued these arrived before this turn (main.c hands
    // the field its key ahead of app_tick), so the guest hears about it ahead
    // of anything that happened during the turn -- and, unlike a JS_Call made
    // from the keystroke itself, on a turn whose job queue is empty.
    pocket_text_pump();
    pocket_imu_pump();
    pocket_io_pump();
    // Before pocket_api_pump(): what the PC answered this turn is posted here
    // and settled below, rather than a frame late.
    pocket_bridge_pump();
    // Same reason: a link that came up or a scan that finished is posted here
    // and settled below, in the turn that noticed it.
    pocket_net_pump();
    // Between the two, so a tone that finished settles in the same order the
    // one pump in pocket_av.c used to settle it in.
    // Before pocket_api_pump(): a read that the microphone can satisfy is
    // filled here and settles below, in the same turn rather than the next.
    pocket_capture_pump();
    pocket_api_pump();
    // AFTER pocket_api_pump(), unlike the ones above it: this one posts no
    // completion for that pump to settle. It delivers fs.onVolumeChange, which
    // says the card was granted or has gone, and a listener may call straight
    // back into pocket.fs -- so it runs in the part of the turn where nothing
    // of that surface is in flight. While the folder picker is up the guest is
    // not ticked at all, so a grant is announced on the first frame after the
    // person chose, which is the first frame the app could act on it.
    pocket_fs_pump();
    pocket_av_pump();
    // The same mask the turn below is handed: pocket.input reports what the
    // host forwarded, never a second reading of the keyboard.
    pocket_ui_pump(buttons);
}

esp_err_t app_tick(uint32_t buttons) {
    // The Back turn is the guest's ONE last chance to save: main.c calls
    // app_tick(0x2000) and requests the stop on the next line, so there is no
    // later turn for a deferred 0x2000 to be delivered on. sec.5.2 says so in
    // as many words -- the leave turn goes on to frame(0x2000) without waiting
    // for the continuation, and is outside the runaway counter -- and it is
    // what VM_LEAVE_BUDGET_US/VM_LEAVE_BACKSTOP exist for. Deferring it
    // instead would drop the save silently, and only for the apps that are
    // busy enough to still have a queue, which is when saving matters most.
    const bool leaving=(buttons&0x2000)!=0;
    turn_continued=false;
    arm_turn(buttons);
    // L1 sec.2.1: a turn that ended with jobs queued finishes them HERE, ahead
    // of every pump. Until the queue is empty no host call reaches JavaScript
    // at all, so the drain the budget cut and its continuations are one
    // logical drain with the same job order the pre-L1 single drain produced.
    //
    //
    // exit() is NOT honoured here, and that is the same rule again: the stop it
    // asks for reaches the guest as an uncatchable interrupt on the next call
    // into JavaScript, so requesting it at the top of a continuation turn would
    // kill job k+1 of a drain that pre-L1 ran to its end -- every .then and
    // every .finally after the exit() would run or not run depending on where
    // the budget happened to fall, which is exactly what a budget boundary must
    // never decide. Pre-L1 the flag was read in pocket_app_pump(), i.e. only on
    // a turn that began with an empty queue, and that is where it is read
    // below. The chain an exiting app is stuck in is bounded by the runaway
    // guard, not by this check.
    if(pocketjs_guest_jobs_pending(guest)) {
        pocketjs_ui_frame_view_t cont={.struct_size=sizeof(cont)};
        // Timed into the same turn_ms as an ordinary turn: a continuation IS a
        // turn as far as the frame period is concerned, and leaving it out
        // would make PAINT's turn_ms report only the cheap turns.
        int64_t cont_began=esp_timer_get_time();
        esp_err_t ce=pocketjs_ui_turn_continue(binding,&cont);
        turn_sum+=(double)(esp_timer_get_time()-cont_began); ticks++;
        if(ce) return ce;
        if(!leaving && pocketjs_guest_jobs_pending(guest)) {
#ifdef CONFIG_POCKET_VM_FAIR
            // FAIR ORDERING (Kconfig POCKET_VM_FAIR, off in the shipping
            // build; docs/vm-L1-report.md sec.10). The drain has yielded with
            // work still queued, and this is the one place compat ordering
            // refuses to let a host event through.
            //
            // THE RULE: the pumps run AFTER the drain has had its budget and
            // only when the queue is still non-empty -- i.e. exactly on the
            // turns compat ordering would have delivered nothing at all. What
            // a pump settles is enqueued by JS_Call'ing a resolve function,
            // and a resolve function APPENDS its reactions to the job queue
            // (ledger 03 fact 53), so the reaction lands BEHIND every job of
            // the unfinished drain: FIFO inside the queue is byte for byte
            // what compat produces. What changes, and the only thing that
            // changes, is that a subscription delivery and a completion's
            // resolve happen at a job boundary in the middle of one logical
            // drain instead of after its end.
            //
            // Draining first rather than pumping first is deliberate: a
            // continuation turn must begin with the continuation, or a
            // high-rate subscription could keep appending work in front of a
            // drain that then never reaches its own budget. It also means no
            // turn ever pumps twice -- if the drain above emptied the queue,
            // control falls through to the ordinary path below, which pumps
            // exactly once, at the same point in the turn as ever.
            //
            // NOT made fair here, and neither is safe to be:
            //   - the exit() check: app_request_stop() is delivered as an
            //     uncatchable interrupt on the next call into JS, so honouring
            //     it at a job boundary cuts the drain it lands in -- whether
            //     a .finally ran would depend on where the budget fell. It
            //     stays below, on a turn that begins with an empty queue.
            //   - frame(): it is the guest's picture, not a host event, and
            //     calling it here would put a frame INSIDE a chain, which
            //     tools/vmtest/corpus/budget_frame_boundary.js exists to
            //     forbid. A continuation turn still shows no frame() in
            //     either mode, so main.c's display pacing (commit 3298d0f) is
            //     untouched: app_turn_continued() is still true here.
            // The unhandled-rejection report point is untouched as well: the
            // guest reports only where vm_sched_drain() returned EMPTY, which
            // is not this boundary.
            buttons|=deferred_buttons; deferred_buttons=0;
            run_pumps(buttons);
#else
            // Nothing new reaches JS this turn. The keys are held, not lost.
            deferred_buttons|=buttons;
#endif
            // sec.5.2. Not an uncatchable throw: the session ends at a job
            // boundary, where no JavaScript frame is live, so this guard
            // cannot skip a finally or strand an await the way the old
            // wall-clock interrupt does.
            if(drain_runaway()) return ESP_ERR_TIMEOUT;
            continuation_turns++;
            turn_continued=true;
            // The display keeps moving: pocketjs_ui_turn_continue() ran the UI
            // core's tick and draw, so `cont` is a real frame to present.
            //
            // At most once per display period, though. main.c no longer
            // charges a continuation turn a frame period (sec.2.4), so these
            // now arrive every ~9 ms, and presenting each one would put four
            // pictures a period on a bus that needs 7.7 ms for one (measured
            // (device)). Skipping costs no drawing: the damage plan is
            // computed against the last COMMITTED target, so everything these
            // turns drew is still in the next present -- this drops frames,
            // it does not lose pixels. The period is kept rather than dropped
            // so that a drain long enough to need many continuations still
            // animates, which is the whole reason a continuation presents.
            if(esp_timer_get_time()-last_present_us < VM_DISPLAY_PERIOD_MS*1000)
                return ESP_OK;
            return present_frame(&cont);
        }
    }
    continuation_turns=0;
    // The queue is empty, so this is the first moment since the exit() that
    // pre-L1 would also have acted on it (pocket_app_pump() read the flag
    // here, ahead of every other pump, and still does everything else it did).
    if(pocket_app_exit_requested()) app_request_stop();
    buttons|=deferred_buttons; deferred_buttons=0;
    run_pumps(buttons);
    pocketjs_ui_input_t input={.struct_size=sizeof(input),.buttons=buttons};
    pocketjs_ui_frame_view_t frame={.struct_size=sizeof(frame)};
    // The JS side of the frame: frame() in QuickJS plus the UI core's tick and
    // draw. Timed on every tick, painted or not, so turn_ms is its own number
    // next to render_ms rather than hidden inside the frame period.
    int64_t turning=esp_timer_get_time();
    esp_err_t e=pocketjs_ui_turn(binding,&input,&frame);
    int64_t turn_us=esp_timer_get_time()-turning;
    turn_sum+=(double)turn_us; ticks++;
#ifdef CONFIG_POCKET_VM_PROBE
    // turn_us is frame() plus whatever job draining pocketjs_ui_turn() does
    // around it -- see vmprobe.h for why the two are not split further.
    vmprobe_frame_sample(guest,turn_us);
#endif
    if(e)return e;
    return present_frame(&frame);
}

// The half of a turn that is not JavaScript: damage plan, strips, bus, and
// the PAINT accounting. Split out for L1 because a CONTINUATION turn has no
// frame() of its own but still has a frame to show -- the UI core ticked and
// drew inside pocketjs_ui_turn_continue() -- and a display frozen for the
// length of a long drain would be a visible regression the level does not
// need to cause.
static esp_err_t present_frame(pocketjs_ui_frame_view_t *frame) {
    esp_err_t e;
    last_present_us=esp_timer_get_time();
    pocketjs_rgb565_damage_plan_t plan={.struct_size=sizeof(plan)};
    e=pocketjs_rgb565_prepare(renderer,target,frame,&plan);if(e)return e;
    pet_assets_tick();
    if(plan.region_count || redraw) {
        redraw=false;
        // Split the same way the home screen is: the renderer's own work
        // against the bytes going down the bus, so there is a number to point
        // at before anyone hand-writes a kernel for either.
        int64_t began=esp_timer_get_time();
        unsigned sent_us=0;
        uint32_t sw_ops=0, accel=0;
        // Full-width strips avoid copying undefined columns of a narrow damage rect.
        uint16_t *pixels=board_strip();
        for(int y=0;y<LCD_H;y+=STRIP_H) {
            int rows=LCD_H-y<STRIP_H?LCD_H-y:STRIP_H;
            memset(pixels,0,(size_t)LCD_W*STRIP_H*sizeof(*pixels));
            pocketjs_rgb565_rect_t region={.x=0,.y=y,.width=LCD_W,.height=rows};
            pocketjs_rgb565_render_stats_t stats={.struct_size=sizeof(stats)};
            e=pocketjs_rgb565_render_strip(renderer,frame,pixels,LCD_W*rows,region,
                                           &render_accel,&stats);
            if(e)goto fail;
            pet_assets_overlay(pixels,y,rows);
            // Section 6's host-owned edit field, composited over the guest's
            // own frame rather than drawn by it: the guest never learns there
            // is a field, only what was committed into it.
            pocket_text_overlay(pixels,y,rows);
            // software_ops counts what the kernels declined, so a non-zero
            // figure here is the share still drawn the slow way.
            sw_ops+=stats.software_ops; accel+=stats.ppa_fills+stats.ppa_blends;
            int64_t sending=esp_timer_get_time();
            e=board_present(y,rows,pixels);if(e)goto fail;
            sent_us+=(unsigned)(esp_timer_get_time()-sending);
        }
        unsigned whole=(unsigned)(esp_timer_get_time()-began);
        render_sum+=whole-sent_us; present_sum+=sent_us; painted++;
        // The kernels' own cycles, over the same 30 frames as the rest, so
        // render_ms splits into what they cost and what the renderer around
        // them costs.
        kernel_sum+=render_accel_cycles; render_accel_cycles=0;
        if(painted==30) {
            ESP_LOGI("app","PAINT turn_ms=%.2f render_ms=%.2f kernel_ms=%.2f send_ms=%.2f accel=%u software=%u",
                     ticks?turn_sum/ticks/1000.0:0.0,
                     render_sum/30/1000.0, kernel_sum/30/240000.0, present_sum/30/1000.0,
                     (unsigned)accel,(unsigned)sw_ops);
            render_sum=0; present_sum=0; kernel_sum=0; painted=0; turn_sum=0; ticks=0;
        }
    }
    e=pocketjs_rgb565_commit(renderer,target,frame);
    frames++;
    if(frames==1)ESP_LOGI("app","HELLO_FRAME_PRESENTED");
    return e;
fail:
    pocketjs_rgb565_abort(renderer,target);return e;
}

