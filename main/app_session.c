#include "app_session.h"
#include "board.h"
#include "pocketjs/guest.h"
#include "pocketjs/guest_quickjs.h"
#include "jsconsole.h"
#include "pocket_api.h"
#include "pocket_random.h"
#include "pocket_storage.h"
#include "pocket_fs.h"
#include "pocket_imu.h"
#include "pocket_av.h"
#include "pocket_av_output_source.h"
#include "pocket_capture.h"
#include "pocket_io.h"
#include "pocket_net.h"
#include "pocket_ble.h"
#include "pocket_text.h"
#include "pocket_app.h"
#include "pocket_clock.h"
#include "pocket_pool_probe.h"
#include "pocket_bridge.h"
#include "pocket_workspace.h"
#include "pocket_overlay.h"
#include "pocket_kasane.h"
#include "ui/kasane/ksn_p0_probe.h"
#include "pocket_input.h"
#include "ksn_font.h"
#include "app_registry.h"
#include "pet_hub.h"
#include "system/sys_device.h"
#include "scene_mem.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "vmprobe.h"
#include "vm_wake.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#ifdef CONFIG_POCKET_VM_SELFTEST
#include "quickjs-vm.h"
// UI-task-only diagnostic state; never expose or mutate JS while it is parked.
static bool vm_storage_active, vm_storage_leaving, vm_storage_parked;
static JSValue vm_finite_done(JSContext *ctx, JSValueConst self,
                              int argc, JSValueConst *argv) {
    (void)self;
    int32_t n;
    double sum;
    if(argc!=2 || JS_ToInt32(ctx,&n,argv[0]) || JS_ToFloat64(ctx,&sum,argv[1]))
        return JS_EXCEPTION;
    ESP_LOGI("app","VM_FINITE_DONE n=%ld sum=%.0f",(long)n,sum);
    return JS_UNDEFINED;
}
static JSValue vm_storage_wait(JSContext *ctx, JSValueConst self,
                               int argc, JSValueConst *argv) {
    (void)self; (void)argc; (void)argv;
    if(!vm_storage_leaving) JS_VMRequestYield(JS_GetRuntime(ctx));
    return JS_NewBool(ctx,!vm_storage_leaving);
}
static JSValue vm_storage_mark(JSContext *ctx, JSValueConst self,
                               int argc, JSValueConst *argv) {
    (void)self;
    int32_t mark=0;
    if(argc && JS_ToInt32(ctx,&mark,argv[0])) return JS_EXCEPTION;
    ESP_LOGI("app","VM_SAVE_MARK %ld",(long)mark);
    return JS_UNDEFINED;
}
#endif

extern const char hello_start[] asm("_binary_main_js_start");
extern const char hello_end[] asm("_binary_main_js_end");
extern const char kasane_demo_start[] asm("_binary_demo_js_start");
#ifdef KASANE_P0_PROBE
extern const char wall_source_probe_start[] asm("_binary_wall_source_probe_js_start");
extern const char pool_source_probe_start[] asm("_binary_pool_source_probe_js_start");
extern const char output_source_probe_start[] asm("_binary_output_source_probe_js_start");
#endif
#ifdef CONFIG_POCKET_VM_PROBE
// VM probe workloads (docs/vm/quickjs-freertos-vm-spec.md sec.5), embedded only
// when this build turned CONFIG_POCKET_VM_PROBE on (main/CMakeLists.txt).
// Reached over USB only -- test chars 'A'..'F' in main.c's usb_stroke() --
// never from the home screen's app list.
extern const char vmp_sync_start[] asm("_binary_sync_loop_js_start");
extern const char vmp_recur_start[] asm("_binary_deep_recursion_js_start");
extern const char vmp_closures_start[] asm("_binary_closures_js_start");
extern const char vmp_promise_start[] asm("_binary_promise_chain_js_start");
extern const char vmp_io_start[] asm("_binary_io_wait_js_start");
extern const char vmp_asyncgen_start[] asm("_binary_async_generator_js_start");
// The contention conditions, applied on top of whichever workload is running.
extern const char vmp_cond_start[] asm("_binary_condition_js_start");
#endif
static pocketjs_guest_t *guest;

// Called after any turn or evaluation that ran JavaScript -- app_tick's two
// paths, app_overlay_tick's, and eval_reporting's parse -- because an
// allocation rejection can happen inside any of them. See JS_TakeOOMCanary
// (quickjs.h): a rejection there can turn into a bare `null` exception once
// JS_ThrowOutOfMemory's own allocation also fails, indistinguishable from the
// script's own `throw null` without this. Not a contracted marker (the
// CLAUDE.md list predates it); a new line costs nothing to add.
static void report_oom_if_any(void) {
    if(!guest) return;
    uint32_t n=0; size_t first_req=0, first_used=0;
    pocketjs_guest_take_oom(guest,&n,&first_req,&first_used);
    if(n>0)
        ESP_LOGE("app","OOM n=%u first_req=%u used=%u",
                 (unsigned)n,(unsigned)first_req,(unsigned)first_used);
}
static atomic_bool stop_requested;
static int64_t deadline;
// L1 (docs/vm/vm-L1-design.md). Armed once per turn and handed to the guest, so
// frame()'s drain and the next turn's continuation drain measure against the
// same turn start.
static vm_budget_t budget;
// Presses that arrived on a turn spent finishing the previous turn's queue.
// pocket_input_pump() delivers into JavaScript and so is held back with the
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
static double render_sum, present_sum, turn_sum;
static unsigned painted, ticks;
// Boundary 7 of docs/perf/kasane-opt-survey.md: the render path's own counts,
// summed over the same 30 frames the millisecond terms cover. Counts only --
// `cy` is rsr.ccount read inside the renderer and 0 with g_ksn_prof off, and
// nothing here is divided by the clock (ksn_render.h says what each field is
// and why a counter is not a duration).
static ksn_render_prof prof_sum;
static unsigned band_count_last, band_runs_last, band_mask_last;
static void prof_accumulate(const ksn_render_prof *frame){
    prof_sum.fill_cy+=frame->fill_cy;prof_sum.fill_n+=frame->fill_n;
    prof_sum.span_cy+=frame->span_cy;prof_sum.span_n+=frame->span_n;
    prof_sum.tile_cy+=frame->tile_cy;prof_sum.tile_n+=frame->tile_n;
    prof_sum.blend_cy+=frame->blend_cy;prof_sum.blend_n+=frame->blend_n;
    prof_sum.read_cy+=frame->read_cy;prof_sum.read_n+=frame->read_n;
}
// Borrowed for the length of a start; the Playground owns the bytes and does
// not edit them while a run is up.
static const char *user_source;
static size_t user_length;
// Evaluated first, in the same realm, when the caller has one. The tutorial's
// counting chapter uses it for the scene and the text it works on.
static const char *user_prelude;
static size_t user_prelude_length;
// Programs written before the legacy UI was removed call ui.createNode and
// ui.setText. Run as they are, they stop on their first line with "ui is not
// defined", which reads like the person's own typo; this names the actual
// reason before a guest is built. A token scan rather than a parse: `ui.` and
// one of the five calls no legacy screen could be built without, preceded by
// neither an identifier character nor a dot -- so pocket.ui, gui.setText and a
// program's own `const ui = pocket.kasane` do not match. A comment that names
// one of the calls matches too: a refusal the person can see and edit away,
// which is the cheaper mistake.
static bool uses_legacy_ui(const char *s, size_t n) {
    static const char *const calls[]={"createNode","setProp","setText","insertBefore","setStyle"};
    for(size_t i=0;i+3<=n;i++) {
        if(s[i]!='u'||s[i+1]!='i'||s[i+2]!='.') continue;
        if(i) {
            unsigned char b=(unsigned char)s[i-1];
            if(b=='.'||b=='_'||b=='$'||b>=0x80||(b>='0'&&b<='9')||
               ((b|0x20)>='a'&&(b|0x20)<='z')) continue;
        }
        for(size_t c=0;c<sizeof calls/sizeof calls[0];c++) {
            size_t len=strlen(calls[c]);
            if(i+3+len<=n&&!memcmp(s+i+3,calls[c],len)) return true;
        }
    }
    return false;
}

// Whether a source can reach pocket.kasane at all. A plain substring and not a
// parse: a hit in a comment only costs that session the arena a little early,
// and a program that builds the name at run time still gets it lazily at its
// first call, only with the heap already split around the guest.
static bool names_kasane(const char *s, size_t n) {
    for(size_t i=0;i+6<=n;i++) if(!memcmp(s+i,"kasane",6)) return true;
    return false;
}

// THE ONE PLACE THE GUEST'S LIFETIME CHANGES.
//
// Until now a session was bounded by entering and leaving an app screen: the
// home screen's loop built a guest when somebody pressed Enter on a row and
// destroyed it when they pressed Back. docs/api/common-api.md 3.1 adds a second
// bound -- the HOME SCREEN owns a session for as long as it is on show -- and
// that is the whole of the difference. Everything else about the contract is
// unchanged and deliberately so: app_stop() below still tears the surfaces
// down in the reverse of the order they were built, and every module holding a
// guest callback is still reset before the guest is destroyed. An overlay
// session is a session; it is only started and ended by a different event.
//
// The flag selects the overlay capability set and shell-owned presentation.
// Kasane still uses the APP lease; its display port receives the live home
// scene as a backdrop instead of clearing to an opaque APP background.
static bool overlay_session;
static bool kasane_presented;
void app_force_redraw(void) { pocket_kasane_invalidate(); }
void app_force_redraw_bands(uint32_t bands) {
    if(bands) pocket_kasane_invalidate_bands(bands);
}
static esp_err_t present_frame(void);
typedef struct { unsigned sent_us; } kasane_display_t;
static uint16_t *kasane_strip(void *opaque) {
    (void)opaque;return board_strip();
}
static ksn_result kasane_send(void *opaque,uint16_t y,uint16_t rows,
                             const uint16_t *pixels) {
    kasane_display_t *display=opaque;
    int64_t began=esp_timer_get_time();
    // Section 6's host-owned edit field, composited over the band the guest's
    // scene has just filled: the guest never learns there is a field, only
    // what was committed into it. Every band is refilled from the scene's
    // background, so nothing drawn here survives into the next frame, and a
    // field that changed invalidates the bands it covers (main.c).
    pocket_text_overlay((uint16_t *)pixels,(int)y,(int)rows);
    // Kasane promotes its command bank only after acknowledged transfers.
    pet_hub_overlay_suppress(pocket_kasane_notice_composited());
    esp_err_t result=board_present_sync(y,rows,(uint16_t *)pixels);
    pet_hub_overlay_suppress(false);
    display->sent_us+=(unsigned)(esp_timer_get_time()-began);
    return result==ESP_OK?KSN_OK:KSN_IO;
}
// The same, for the damaged columns of a band. The field is painted first here
// too: it draws its whole box into the strip and only the window goes out, and
// what it drew outside the window is already on the glass from the full-width
// frame that put it there.
static ksn_result kasane_send_rect(void *opaque,uint16_t x,uint16_t y,uint16_t cols,
                                   uint16_t rows,const uint16_t *pixels) {
    kasane_display_t *display=opaque;
    int64_t began=esp_timer_get_time();
    pocket_text_overlay((uint16_t *)pixels,(int)y,(int)rows);
    pet_hub_overlay_suppress(pocket_kasane_notice_composited());
    esp_err_t result=board_present_rect_sync(x,y,cols,rows,(uint16_t *)pixels);
    pet_hub_overlay_suppress(false);
    display->sent_us+=(unsigned)(esp_timer_get_time()-began);
    return result==ESP_OK?KSN_OK:KSN_IO;
}

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

void app_vm_prepare_stop(void) {
    if(guest) pocketjs_guest_prepare_stop(guest);
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
    pocketjs_guest_yield_enabled(guest,(buttons&0x2000)==0);
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
    if(pocketjs_guest_frame_total(guest)>=VM_FRAME_RUNAWAY_US) {
        ESP_LOGE("app","RUNAWAY one frame spent %lld us",
                 (long long)pocketjs_guest_frame_total(guest));
        jsconsole_set_error("FRAME RUNAWAY");
        return true;
    }
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
        // Parsing counts too (CLAUDE.md: source bytes eat the guest's heap
        // before a single line runs), so a source too big to parse can throw
        // this same bare null. Reported here rather than folded into
        // EVAL_ERROR's own text: that marker's format is read by nothing
        // today but is exactly the shape test_settings.py etc. treat as
        // contracted, so a new field goes on a line of its own.
        report_oom_if_any();
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
    bool p0_had_guest=guest!=NULL;
#ifdef CONFIG_POCKET_VM_PROBE
    vmprobe_session_reset();
#endif
    // Before the guest goes: the watches hold callbacks belonging to it, and a
    // promise still in flight holds its resolvers.
    // First: section 5 runs the stop hook before I/O cancellation and before
    // the subscriptions it may still want to use are taken away.
    pocket_app_reset();
    pocket_imu_reset();
#ifdef KASANE_P0_PROBE
    /* Read before pocket_av_reset() stops the stream and clears the player.
     * Decoder faults are distinct from audio output underruns. */
    int32_t p0_player=pocket_av_ui_current_player();
    pocket_av_ui_snapshot p0_audio;
    if(p0_player&&pocket_av_ui_read(p0_player,&p0_audio))
        ESP_LOGI("KSN_P0","A session=%s player=%ld state=%u position_ms=%lu underruns=%lu",
            overlay_session?"overlay":"app",(long)p0_player,(unsigned)p0_audio.state,
            (unsigned long)p0_audio.position_ms,(unsigned long)p0_audio.underruns);
#endif
    bool av_stopped=pocket_av_reset();
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
    if(pocket_kasane_reset()){
        pocket_clock_reset();
        pocket_av_output_source_reset(av_stopped);
#ifdef KASANE_P0_PROBE
        pocket_pool_probe_reset();
#endif
    }
    pocket_input_reset();
    pocket_overlay_reset();
    // Before pocket_api_reset(): an open field holds three guest callbacks, and
    // a screen change closes the session -- which is what the end of a run is.
    pocket_text_reset();
    pocket_bridge_reset();
    pocket_api_reset();
    // Read before the runtime goes: app_report() below runs with guest == NULL,
    // so this is the last point at which "was anything still queued" has an
    // answer. pocket_app_reset() above has already given the stop hook its
    // 200 ms, so what is pending here is what really gets discarded.
    if(guest) {
        final_stats=(pocketjs_guest_stats_t){.struct_size=sizeof(final_stats)};
        pocketjs_guest_stats(guest,&final_stats);
    }
    if(guest) pocketjs_guest_destroy(guest);
    guest=NULL;
    app_report();
    if(p0_had_guest)ksn_p0_probe_report(overlay_session?"overlay":"app");
#ifdef KASANE_P0_BUS_PROBE
    if(p0_had_guest)ESP_LOGI("board","P1 LCD ISR observed core %d at app stop",
                            board_lcd_isr_core());
#endif
    ESP_LOGI("app","APP_STOPPED");
}
esp_err_t app_start_test(char test) {
    esp_err_t err;
#ifdef CONFIG_POCKET_VM_SELFTEST
    vm_storage_active=test=='Y';
    vm_storage_leaving=vm_storage_parked=false;
#endif
    // The USB diagnostics call this directly, so they are the one caller that
    // has not set these three. Left over from the boot overlay, overlay_session
    // skipped the test's source and its renderer, and the session died on its
    // first tick with no error line -- every diagnostic run after boot did.
    if(test) { user_source=NULL; user_prelude=NULL; overlay_session=false; }
    kasane_presented=false;
    ksn_p0_probe_reset();
    // Refused before a guest exists, so a program that cannot run costs nothing.
    if(user_source && !overlay_session && uses_legacy_ui(user_source,user_length)) {
        jsconsole_set_error("旧API(ui.*)のため実行できません");
        ESP_LOGW("app","APP_LEGACY_UI %u bytes",(unsigned)user_length);
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* Foreground ownership ends the background scratch lifetime on every
     * entry path, including USB diagnostics. Overlays still share the scene. */
    if(!overlay_session)scene_mem_release();
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
    TRY(vmprobe_segment_apply(guest) == 0 ? ESP_OK : ESP_ERR_INVALID_STATE);
    vmprobe_static_report();
#endif
    pocketjs_guest_set_watchdog(guest,interrupt,NULL);
    // Replaces quickjs-libc's print, whose output only ever reaches stdout.
    jsconsole_clear();
    TRY(pocketjs_guest_quickjs_install_once(guest,"console",jsconsole_install,NULL));
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
        TRY(pocketjs_guest_quickjs_install_once(guest,"kasane",pocket_kasane_install,NULL));
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
    TRY(pocketjs_guest_quickjs_install_once(guest,"kasane",pocket_kasane_install,NULL));
    TRY(pocketjs_guest_quickjs_install_once(guest,"input",pocket_input_install,NULL));
    // After "input": both contribute to pocket.input, and contributors run in
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
    // pocket.pet is the surface of two native apps, Pocket Pet and Pet
    // Companion, not part of the common API. It goes only to a session whose
    // manifest names pet.companion, so no other app can reach the pet's
    // notifications, timers or NVS through it (docs/api/common-api.md section 2).
    if(app_registry_wants(app_registry_current(),"pet.companion"))
        TRY(pocketjs_guest_quickjs_install_once(guest,"pet-hub",pet_hub_install,NULL));
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
#ifdef CONFIG_POCKET_VM_SELFTEST
        case '[': case '\\': case ']': source=
            "let ran=false;globalThis.frame=b=>{if(ran||(b&8192))return;ran=true;"
            "let s=0,n=vmFiniteN;for(let i=0;i<n;i++)s+=i;vmFiniteDone(n,s)};";
            break;
        // Dedicated owner and create-only writes keep diagnostics out of the
        // selected app's store. A pre-existing test key is never overwritten.
        case 'Y': source=
            "let ready=false,saved=false;const key='back-save-20260916';"
            "pocket.storage.get(key).then(r=>{if(r!==null)throw Error('test key exists');"
            "ready=true;vmSaveMark(1)}).catch(()=>vmSaveMark(9));"
            "pocket.app.start({stop:()=>{if(!saved){vmSaveMark(9);return}"
            "vmSaveMark(4);return Promise.resolve().then(()=>vmSaveMark(5))}});"
            "globalThis.frame=b=>{if(!(b&8192)){"
            "if(ready)while(vmStorageWait()){}return}"
            "if(!ready){vmSaveMark(9);return}ready=false;vmSaveMark(2);"
            "return pocket.storage.set(key,{token:'vm-back-20260916',back:b},"
            "{ifRevision:0}).then(()=>{saved=true;vmSaveMark(3)})"
            ".catch(()=>vmSaveMark(9))};";
            break;
        case 'Z': source=
            "const key='back-save-20260916';pocket.storage.get(key).then(r=>{"
            "if(!r||r.revision!==1||JSON.stringify(r.value)!=="
            "'{\"token\":\"vm-back-20260916\",\"back\":8192}')"
            "throw Error('unexpected test record');vmSaveMark(6);"
            "return pocket.storage.remove(key)}).then(()=>{vmSaveMark(7);"
            "return pocket.storage.get(key)}).then(r=>{if(r!==null)"
            "throw Error('test record remains');vmSaveMark(8)})"
            ".catch(()=>vmSaveMark(9));globalThis.frame=()=>{};";
            break;
        case 'M': source=
            "pocket.app.start({stop:()=>{vmMark(5);return Promise.resolve().then(()=>vmMark(6))}});"
            "function work(){vmMark(1);vmRequest();for(let i=0;i<3;i++){}vmMark(2)}"
            "globalThis.frame=b=>{if(b&8192){vmMark(3);Promise.resolve().then(()=>vmMark(4));return}"
            "if(vmMode===0)work();else if(vmMode===1)Promise.resolve().then(work);"
            "else return (async()=>{await 0;work()})()};";
            break;
#endif
        // The Kasane demo (ui/kasane via app_session.c's kasane_demo_start): the
        // one source that is not a VM self-test, so it sits outside the ifdef.
        case 'K': source=kasane_demo_start; break;
#ifdef KASANE_P0_PROBE
        case '7': source=wall_source_probe_start; break;
        case '0': source=pool_source_probe_start; break;
        case 'v': source=output_source_probe_start; break;
#endif
#ifdef CONFIG_POCKET_VM_PROBE
        // VM probe workloads (sec.5): real files under apps/vmprobe/ rather
        // than inline strings like '1'..'6' above, because
        // tools/vm_l0_capture.py wants named, reviewable sources and because
        // their byte count is itself part of what the probe costs the guest
        // heap. Only `source` is set: TEXT embeds are NUL-terminated, so the
        // strlen() below is exact for them too, and that line stays the same
        // instruction in a probe-off build (a `test<'A'` guard here once cost
        // the off build 20 B flash).
        case 'A': source=vmp_sync_start; break;
        case 'X': source=hello_start; break;
        case 'B': source=vmp_recur_start; break;
        case 'C': source=vmp_closures_start; break;
        case 'D': source=vmp_promise_start; break;
        case 'E': source=vmp_io_start; break;
        case 'F': source=vmp_asyncgen_start; break;
#endif
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
#ifdef CONFIG_POCKET_VM_SELFTEST
    if(test=='['||test=='\\'||test==']') {
        JSContext *ctx=pocketjs_guest_quickjs_context(guest);
        JSValue global=JS_GetGlobalObject(ctx);
        int n=test=='['?20000:test=='\\'?40000:100000;
        int installed=JS_SetPropertyStr(ctx,global,"vmFiniteN",JS_NewInt32(ctx,n));
        installed|=JS_SetPropertyStr(ctx,global,"vmFiniteDone",
                                    JS_NewCFunction(ctx,vm_finite_done,"vmFiniteDone",2));
        JS_FreeValue(ctx,global);
        if(installed<0) { err=ESP_ERR_NO_MEM; goto fail; }
#ifdef CONFIG_POCKET_VM_YIELD
        pocketjs_guest_trace_frame(guest);
#endif
    }
    if(test=='Y'||test=='Z') {
        pocket_storage_set_owner("vm.back.selftest.20260916");
        JSContext *ctx=pocketjs_guest_quickjs_context(guest);
        JSValue global=JS_GetGlobalObject(ctx);
        int installed=JS_SetPropertyStr(ctx,global,"vmSaveMark",
                                       JS_NewCFunction(ctx,vm_storage_mark,"vmSaveMark",1));
        installed|=JS_SetPropertyStr(ctx,global,"vmStorageWait",
                                    JS_NewCFunction(ctx,vm_storage_wait,"vmStorageWait",0));
        JS_FreeValue(ctx,global);
        if(installed<0) { err=ESP_ERR_NO_MEM; goto fail; }
    }
#endif
    // The native Kasane arena is ~9.9 KiB. Taken at the guest's first Kasane
    // call it lands between allocations the guest has just made and splits the
    // largest free block; taken here, before evaluation, it is one block from
    // an unbroken heap (docs/kasane/kasane-guest-memory-reduce.md). A source
    // that never names it, including a compatibility overlay, pays nothing.
    if(names_kasane(source,length)||
       (user_prelude&&names_kasane(user_prelude,user_prelude_length)))
        pocket_kasane_prepare();
    if(user_source) err=eval_user_source(source,length);
    else err=pocketjs_guest_eval(guest,source,length,test?"diagnostic.js":"hello.js");
    pocket_kasane_end_turn();
    if(err!=ESP_OK)goto fail;
#ifdef CONFIG_POCKET_VM_PROBE
    // sec.5's fixed contention conditions, applied to a probe workload only.
    // Two evaluations rather than one concatenated source: the mask is a
    // number the host chose at run time, and pocketjs_guest_eval() re-reads
    // globalThis.frame after each one, which is what lets condition.js wrap
    // the workload's frame() and have the wrapper actually be called.
    //
    // The condition script keeps its own failures to itself (it logs a VMCOND
    // line and continues), so a Wi-Fi that will not link degrades the
    // condition and is recorded, instead of ending the session.
    if(test>='A'&&test<='F') {
        unsigned mask=vmprobe_condition();
        ESP_LOGI("app","VMCOND start mask=%u",mask);
        if(mask) {
            char select[32];
            int n=snprintf(select,sizeof select,"globalThis.VMC=%u;",mask);
            TRY(pocketjs_guest_eval(guest,select,(size_t)n,"vmcond-select.js"));
            TRY(pocketjs_guest_eval(guest,vmp_cond_start,strlen(vmp_cond_start),
                                    "condition.js"));
        }
    }
#endif
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

// One turn of an overlay. Guest work and Kasane transaction finalization live
// here; strip composition and the LCD bus remain in shell_draw(), where the
// native scene can be supplied as the backdrop.
esp_err_t app_overlay_tick(void) {
    if(!guest) return ESP_ERR_INVALID_STATE;
    pocket_kasane_set_animation_time((uint64_t)esp_timer_get_time());
    /* Presentation and repair own the retained candidate. Do not let another
     * guest turn race it; shell_draw() will retry it later in this frame. */
    if(pocket_kasane_needs_present())return ESP_OK;
    bool presenter_blocked=false;
    ksn_result presenter_result=pocket_kasane_presenter_step(&presenter_blocked);
    if(presenter_result!=KSN_OK){
        ESP_LOGE("kasane","PRESENTER_STEP_FAILED %u",(unsigned)presenter_result);
        return ESP_FAIL;
    }
    if(presenter_blocked)return ESP_OK;
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
    // overlay does not install. The drain is resumed directly.
    if(pocketjs_guest_work_pending(guest)) {
        esp_err_t ce=pocketjs_guest_continue(guest);
        pocket_kasane_end_turn();
        report_oom_if_any();
#ifdef CONFIG_POCKET_VM_PROBE
        vmprobe_continuation_sample(guest);
#endif
        if(ce) return ce;
        if(pocketjs_guest_work_pending(guest)) {
#ifdef CONFIG_POCKET_VM_FAIR
            if(!pocketjs_guest_suspended(guest)) {
            // Fair ordering, the overlay's share of it: the same rule and the
            // same reasons as app_tick() states at length, over the pumps an
            // overlay session actually installs. No exit() check and no
            // frame() here either.
            pocket_app_pump();
            pocket_overlay_pump();
            pocket_api_pump();
            pocket_fs_pump();
            pocket_av_pump();
            }
#endif
            if(drain_runaway()) return ESP_ERR_TIMEOUT;
            continuation_turns++;
            // No new submission this turn: the shell recomposites the retained
            // command bank over the current native scene.
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
    pocket_kasane_end_turn();
    frames++;
    report_oom_if_any();
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
    pocket_input_pump(buttons);
}

// One call into the guest. The Cardputer supplies a pad mask, centered analog
// axes and no touches; whatever the guest drew is already in Kasane's bank.
static esp_err_t dispatch_guest(bool continuing,uint32_t buttons) {
    const pocketjs_guest_frame_t input={.struct_size=sizeof(input),
                                       .buttons=buttons,.analog=0x8080};
    return continuing?pocketjs_guest_continue(guest):
                      pocketjs_guest_frame(guest,&input);
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
    turn_continued=false;
    pocket_kasane_set_animation_time((uint64_t)esp_timer_get_time());
    bool presenter_blocked=false;
    ksn_result presenter_result=pocket_kasane_presenter_step(&presenter_blocked);
    if(presenter_result!=KSN_OK){
        ESP_LOGE("kasane","PRESENTER_STEP_FAILED %u",(unsigned)presenter_result);
        return ESP_FAIL;
    }
    sys_notice notice;
    bool have_notice=sys_notify_active(sys_device_notifications(),&notice);
    /* BUSY/limits leave the legacy overlay available until SYSTEM can submit.
     * The next owner turn retries without interrupting the guest. */
    (void)pocket_kasane_update_notice(have_notice?&notice:NULL,pet_hub_selected());
    // A top-level replace() is submitted while the source is evaluated, before
    // there is a PocketJS frame to hand to present_frame(). Present that image
    // as its own owner turn. The same gate retries a partial LCD transfer
    // without letting another JS update race repair. An invalidated committed
    // screen also reaches this gate when no JS submission exists.
    if(pocket_kasane_needs_present()) {
        bool guest_submission=pocket_kasane_has_submission()&&!pocket_kasane_animation_pending()&&!pocket_kasane_system_pending();
        esp_err_t pending=present_frame();
        if(pending!=ESP_OK)return pending;
        // A completed owner-only redraw must allow this tick's JS turn. Live
        // indicators can invalidate every tick; returning here unconditionally
        // would starve the guest for the entire recording. Guest submissions
        // retain their existing dedicated display turn, and IO keeps retrying.
        if(!(buttons&0x2000)&&(guest_submission||pocket_kasane_needs_present()))
            return ESP_OK;
        // Back is host-priority and this is the guest's final save turn. Carry
        // it into JS even if LCD IO still needs retry, so display trouble cannot
        // swallow the final save. Other input during the blocked
        // interval is deliberately dropped rather than replayed.
    }
    if(!(buttons&0x2000)&&pocket_kasane_input_scope(false)==KSN_INPUT_BLOCKED)
        buttons=0;
    const bool leaving=(buttons&0x2000)!=0;
#ifdef CONFIG_POCKET_VM_SELFTEST
    // The selftest's Back-path mark: taken where the turn's own `leaving` is
    // decided (the VM self-test cares that the mark lands on the leave turn,
    // not which renderer is behind it).
    if(leaving && vm_storage_active) {
        ESP_LOGI("app","VM_SAVE_MARK %d",pocketjs_guest_suspended(guest)?11:9);
        vm_storage_leaving=true;
    }
#endif
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
    if(pocketjs_guest_work_pending(guest)) {
        // Timed into the same turn_ms as an ordinary turn: a continuation IS a
        // turn as far as the frame period is concerned, and leaving it out
        // would make PAINT's turn_ms report only the cheap turns.
        int64_t cont_began=esp_timer_get_time();
        esp_err_t ce=dispatch_guest(true,0);
        pocket_kasane_end_turn();
        uint32_t continuation_us=(uint32_t)(esp_timer_get_time()-cont_began);
        turn_sum+=(double)continuation_us; ticks++;
        ksn_p0_probe_sample(KSN_P0_APP_TURN,continuation_us);
        report_oom_if_any();
#ifdef CONFIG_POCKET_VM_PROBE
        vmprobe_continuation_sample(guest);
#endif
        if(ce) return ce;
        if(!leaving && pocketjs_guest_work_pending(guest)) {
#ifdef CONFIG_POCKET_VM_FAIR
            // FAIR ORDERING (Kconfig POCKET_VM_FAIR, off in the shipping
            // build; docs/vm/vm-L1-report.md sec.9). The drain has yielded with
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
            if(!pocketjs_guest_suspended(guest)) {
                buttons|=deferred_buttons; deferred_buttons=0;
                run_pumps(buttons);
            } else {
                // A suspended chain is the selftest's storage park: hold the keys
                // for the turn that can run them instead of dropping them.
                deferred_buttons|=buttons;
            }
            pocket_kasane_end_turn();
#else
            // Nothing new reaches JS this turn. The keys are held, not lost.
            deferred_buttons|=buttons;
#endif
            // sec.5.2 plus the L2c frame guard: a timed-out suspended chain
            // is terminated by pocket_app_reset() before stop-hook JS entry.
            // A job boundary has no live chain; an opcode park can still have
            // one, and termination there deliberately skips its finally.
            if(drain_runaway()) return ESP_ERR_TIMEOUT;
            continuation_turns++;
            turn_continued=true;
            // The display keeps moving: dispatch_guest updated the selected
            // backend, so the owner can present the latest state.
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
            return present_frame();
        }
    }
    continuation_turns=0;
    // The queue is empty, so this is the first moment since the exit() that
    // pre-L1 would also have acted on it (pocket_app_pump() read the flag
    // here, ahead of every other pump, and still does everything else it did).
    if(pocket_app_exit_requested()) app_request_stop();
    buttons|=deferred_buttons; deferred_buttons=0;
    run_pumps(buttons);
    pocket_kasane_end_turn();
    // The JS side of the frame: frame() in QuickJS. Timed on every tick, painted or not, so turn_ms is its own number
    // next to render_ms rather than hidden inside the frame period.
    int64_t turning=esp_timer_get_time();
    esp_err_t e=dispatch_guest(false,buttons);
    pocket_kasane_end_turn();
#ifdef CONFIG_POCKET_VM_SELFTEST
    // The storage park, marked where the tick can see it: the guest suspended
    // itself mid-turn and the next turn is what runs its continuation.
    if(vm_storage_active && !vm_storage_parked && pocketjs_guest_suspended(guest)) {
        vm_storage_parked=true;
        ESP_LOGI("app","VM_SAVE_MARK 10");
    }
#endif
    int64_t turn_us=esp_timer_get_time()-turning;
    turn_sum+=(double)turn_us; ticks++;
    ksn_p0_probe_sample(KSN_P0_APP_TURN,(uint32_t)turn_us);
    report_oom_if_any();
#ifdef CONFIG_POCKET_VM_PROBE
    // turn_us is frame() plus whatever job draining dispatch_guest() does
    // around it -- see vmprobe.h for why the two are not split further.
    vmprobe_frame_sample(guest,turn_us);
#endif
    if(e)return e;
    return present_frame();
}

#ifdef CONFIG_POCKET_VM_SELFTEST
// Exercise the production Back path without writing user storage. The marks
// stand for the save and its completion; they must precede both stop-hook parts.
static unsigned vm_back_trace;
static JSValue vm_back_mark(JSContext *ctx, JSValueConst self,
                            int argc, JSValueConst *argv) {
    (void)self;
    int32_t mark=0;
    if(argc && JS_ToInt32(ctx,&mark,argv[0])) return JS_EXCEPTION;
    vm_back_trace=vm_back_trace*10+(unsigned)mark;
    return JS_UNDEFINED;
}
static JSValue vm_back_request(JSContext *ctx, JSValueConst self,
                               int argc, JSValueConst *argv) {
    (void)self; (void)argc; (void)argv;
    JS_VMRequestYield(JS_GetRuntime(ctx));
    return JS_UNDEFINED;
}
void app_vm_back_selftest(void) {
    bool ok=true;
    const unsigned before=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    for(int mode=0;mode<3;mode++) for(int aborting=0;aborting<2;aborting++) {
        vm_back_trace=0;
        esp_err_t err=app_start_test('M');
        if(!err) {
            JSContext *ctx=pocketjs_guest_quickjs_context(guest);
            JSValue global=JS_GetGlobalObject(ctx);
            int installed=JS_SetPropertyStr(ctx,global,"vmMode",JS_NewInt32(ctx,mode));
            installed|=JS_SetPropertyStr(ctx,global,"vmMark",JS_NewCFunction(ctx,vm_back_mark,"vmMark",1));
            installed|=JS_SetPropertyStr(ctx,global,"vmRequest",JS_NewCFunction(ctx,vm_back_request,"vmRequest",0));
            JS_FreeValue(ctx,global);
            if(installed<0) err=ESP_FAIL;
            if(!err) err=app_tick(0);
            if(!err && (!pocketjs_guest_suspended(guest) || vm_back_trace!=1)) err=ESP_FAIL;
            if(!err && !aborting) err=app_tick(0x2000);
        }
        app_request_stop();
        app_stop();
        const unsigned expected=aborting?156:123456;
        bool passed=!err && vm_back_trace==expected;
        ESP_LOGI("app","VM_BACK mode=%d abort=%d trace=%u expected=%u err=%d %s",
                 mode,aborting,vm_back_trace,expected,err,passed?"OK":"FAIL");
        ok &= passed;
    }
    const unsigned after=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    ESP_LOGI("app","VM_BACK_%s free_before=%u free_after=%u",
             ok?"OK":"FAIL",before,after);
}
#endif

// The half of a turn that is not JavaScript: Kasane's damage bands, the bus,
// and the KASANE_PAINT accounting. Split out for L1 because a CONTINUATION
// turn has no frame() of its own but still has a picture to show -- whatever
// its jobs submitted -- and a display frozen for the length of a long drain
// would be a visible regression the level does not need to cause.
static esp_err_t present_frame(void) {
    last_present_us=esp_timer_get_time();
    {
        ksn_result advanced=pocket_kasane_advance((uint64_t)esp_timer_get_time());
        if(advanced!=KSN_OK&&advanced!=KSN_BUSY)return ESP_FAIL;
        kasane_display_t display_state={0};
        // Offering present_rect is what lets the renderer composite only the
        // damaged columns of a band. It is withdrawn while board_capture is
        // dumping rows, because those PIX lines print all 240 columns of the
        // shared strip and only the window would have been repainted.
        ksn_display_port port={.ctx=&display_state,.strip=kasane_strip,.present=kasane_send,
                              .width=LCD_W,.height=LCD_H,.strip_rows=STRIP_H,.text=&ksn_font_port,
                              .present_rect=board_capture_active()?NULL:kasane_send_rect};
        ksn_render_stats stats;
        int64_t began=esp_timer_get_time();
        ksn_result result=pocket_kasane_present(&port,&stats);
        if(result==KSN_OK)pocket_kasane_animations_presented((uint64_t)esp_timer_get_time());
        unsigned whole=(unsigned)(esp_timer_get_time()-began);
        frames++;
        if(result==KSN_IO) {
            ESP_LOGW("kasane","LCD transfer failed; retaining display work for retry");
            return ESP_OK;
        }
        if(result!=KSN_OK) {
            ESP_LOGE("kasane","present failed: %u",(unsigned)result);
            return ESP_FAIL;
        }
        if(stats.bands) {
            painted++;render_sum+=whole-display_state.sent_us;
            present_sum+=display_state.sent_us;
            ksn_p0_probe_sample(KSN_P0_APP_RENDER,whole-display_state.sent_us);
            ksn_p0_probe_sample(KSN_P0_APP_SEND,display_state.sent_us);
            ksn_p0_probe_transfer(stats.transferred_bytes,ksn_render_band_count(stats.bands));
            // This frame's counts into the window's. The millisecond columns on
            // the line are averages and these are sums, which the line says with
            // `frames=`; the renderer resets its accumulators on read, so the
            // window cannot inherit a frame from the last one.
            {ksn_render_prof frame_prof;ksn_render_prof_read(&frame_prof);prof_accumulate(&frame_prof);}
            // Band distribution of the frame whose `bytes=` is printed. `bands`
            // is a count and `band_runs` the number of contiguous runs, because
            // 7 bands of 8 rows and 14 of 4 leave the same bytes behind and the
            // send average cannot tell them apart (kasane-opt-survey.md 10.1).
            band_mask_last=stats.bands;
            band_count_last=ksn_render_band_count(stats.bands);
            band_runs_last=ksn_render_band_runs(stats.bands);
            // Session-scoped rather than process-scoped: design-contracts resets
            // the flag when a kasane session starts.
            if(!kasane_presented){kasane_presented=true;ESP_LOGI("kasane","KASANE_FRAME_PRESENTED");}
            if(painted==30) {
                ESP_LOGI("kasane","KASANE_PAINT turn_ms=%.2f render_ms=%.2f send_ms=%.2f "
                         "bytes=%u bands=%u band_runs=%u band_mask=0x%05x prof=%d "
                         "fill_n=%u fill_cy=%u span_n=%u span_cy=%u tile_n=%u tile_cy=%u "
                         "blend_n=%u blend_cy=%u read_n=%u read_cy=%u frames=%u",
                         ticks?turn_sum/ticks/1000.0:0.0,render_sum/30/1000.0,
                         present_sum/30/1000.0,(unsigned)stats.transferred_bytes,
                         band_count_last,band_runs_last,band_mask_last,g_ksn_prof,
                         (unsigned)prof_sum.fill_n,(unsigned)prof_sum.fill_cy,
                         (unsigned)prof_sum.span_n,(unsigned)prof_sum.span_cy,
                         (unsigned)prof_sum.tile_n,(unsigned)prof_sum.tile_cy,
                         (unsigned)prof_sum.blend_n,(unsigned)prof_sum.blend_cy,
                         (unsigned)prof_sum.read_n,(unsigned)prof_sum.read_cy,painted);
#ifndef KASANE_AB
#define KASANE_AB 0
#endif
#if KASANE_AB
                // Same-binary A/B, one window per arm, off in the shipping
                // build -- shell.c's SCENE_AB is the same shape for the home
                // screen. The arms rotate the render path's runtime switches one
                // at a time: every "off" window has an all-on neighbour on each
                // side in the same scene, so the paired difference is that one
                // switch, and two builds are not a measurement
                // (docs/perf/pie-simd.md 6.2, 6.3). Each optimization appends one
                // row; the line carries this window's own counts next to the
                // terms that must not move, which are its control column. In the
                // prof-off arm the count columns read 0 by construction, because
                // that arm is what defines the control.
                {
                    static const struct {const char *name;int *flag;} switches[]={
                        {"prof",&g_ksn_prof},
                        /* One row per optimisation of the perf/kasane-opt integration, so
                         * one binary compares each switch against an all-on window on
                         * either side: render_ms is the render path's own cost and the
                         * control columns (bands/fill/span/tile/blend/read) show that
                         * nothing else moved. rows+1 arms, a window each, so a switch's
                         * off window is ~1 s from its all-on neighbours. */
                        {"row_cov",&g_ksn_row_coverage},
                        {"row_tbl",&g_ksn_row_table},
                        {"lut",&g_ksn_blend_lut},
                        {"lut_alpha",&g_ksn_blend_lut_alpha},
                        {"affine",&g_ksn_group_affine},
                        {"scale256",&g_ksn_scale256},
                        {"pie",&g_ksn_blend_pie},
                        {"tile_reach",&g_ksn_tile_reach},
                        {"tile_smooth",&g_ksn_tile_smooth},
                        {"decode1",&g_ksn_decode_once},
                        {"span_narrow",&g_ksn_span_narrow},
                    };
                    const unsigned rows=sizeof(switches)/sizeof(switches[0]);
                    static unsigned ab_arm;
                    const unsigned arm=ab_arm%(rows+1);
                    char states[256]={0};
                    for(unsigned r=0;r<rows;r++) {
                        char one[24];
                        snprintf(one,sizeof(one),"%s%s=%d",r?" ":"",switches[r].name,*switches[r].flag);
                        strncat(states,one,sizeof(states)-strlen(states)-1);
                    }
                    ESP_LOGI("kasane","AB arm=%u %s turn_ms=%.2f render_ms=%.2f send_ms=%.2f "
                             "bands=%u band_runs=%u fill_n=%u span_n=%u tile_n=%u blend_n=%u "
                             "read_n=%u frames=%u",
                             arm,states,ticks?turn_sum/ticks/1000.0:0.0,render_sum/30/1000.0,
                             present_sum/30/1000.0,band_count_last,band_runs_last,
                             (unsigned)prof_sum.fill_n,(unsigned)prof_sum.span_n,
                             (unsigned)prof_sum.tile_n,(unsigned)prof_sum.blend_n,
                             (unsigned)prof_sum.read_n,painted);
                    for(unsigned r=0;r<rows;r++)*switches[r].flag=arm!=r+1;
                    ab_arm++;
                }
#endif
                render_sum=0;present_sum=0;painted=0;turn_sum=0;ticks=0;
                prof_sum=(ksn_render_prof){0};
            }
        }
        return ESP_OK;
    }
}
