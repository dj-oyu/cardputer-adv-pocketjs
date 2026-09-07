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
#include "pocket_storage.h"
#include "pocket_fs.h"
#include "pocket_imu.h"
#include "pocket_av.h"
#include "pocket_io.h"
#include "pocket_net.h"
#include "pocket_ble.h"
#include "pocket_ui.h"
#include "pocket_text.h"
#include "pocket_app.h"
#include "pocket_bridge.h"
#include "pocket_workspace.h"
#include "app_registry.h"
#include "pet_assets.h"
#include "pet_hub.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
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
void app_force_redraw(void) { redraw=true; }

static int interrupt(JSRuntime *rt, void *opaque) {
    (void)rt; (void)opaque;
    return atomic_load(&stop_requested) || esp_timer_get_time()>deadline;
}
static esp_err_t install_limits(JSContext *ctx, void *data) {
    (void)data;
    JS_SetInterruptHandler(JS_GetRuntime(ctx),interrupt,NULL);
    return ESP_OK;
}
void app_request_stop(void) { atomic_store(&stop_requested,true); }

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
void app_report(void) {
    pocketjs_guest_stats_t stats={.struct_size=sizeof(stats)};
    if(guest) pocketjs_guest_stats(guest,&stats);
    ESP_LOGI("app","MEM free=%u largest=%u js=%u frames=%u",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
        (unsigned)stats.heap_used,frames);
}
void app_stop(void) {
    jsfont_detach();
    // Before the guest goes: the watches hold callbacks belonging to it, and a
    // promise still in flight holds its resolvers.
    // First: section 5 runs the stop hook before I/O cancellation and before
    // the subscriptions it may still want to use are taken away.
    pocket_app_reset();
    pet_assets_reset();
    pocket_imu_reset();
    pocket_av_reset();
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
    // Before pocket_api_reset(): an open field holds three guest callbacks, and
    // a screen change closes the session -- which is what the end of a run is.
    pocket_text_reset();
    pocket_bridge_reset();
    pocket_api_reset();
    if(renderer && target) pocketjs_rgb565_abort(renderer,target);
    if(target) pocketjs_rgb565_target_destroy(target);
    if(renderer) pocketjs_rgb565_renderer_destroy(renderer);
    if(guest) pocketjs_guest_destroy(guest);
    if(binding) pocketjs_ui_qjs_destroy(binding);
    if(core) pocketjs_ui_core_destroy(core);
    target=NULL;renderer=NULL;guest=NULL;binding=NULL;core=NULL;
    app_report();
    ESP_LOGI("app","APP_STOPPED");
}
esp_err_t app_start_test(char test) {
    esp_err_t err;
    atomic_store(&stop_requested,false); frames=0;
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
#define TRY(expr) do {err=(expr);if(err!=ESP_OK)goto fail;}while(0)
    TRY(pocketjs_guest_create(&gc,&guest));
    TRY(pocketjs_guest_quickjs_install(guest,install_limits,NULL));
    // Replaces quickjs-libc's print, whose output only ever reaches stdout.
    jsconsole_clear();
    TRY(pocketjs_guest_quickjs_install_once(guest,"console",jsconsole_install,NULL));
    TRY(pocketjs_guest_quickjs_install_once(guest,"jsfont",jsfont_install,NULL));
    TRY(pocketjs_guest_quickjs_install_once(guest,"pocket",pocket_api_install,NULL));
    TRY(pocketjs_guest_quickjs_install_once(guest,"storage",pocket_storage_install,NULL));
    TRY(pocketjs_guest_quickjs_install_once(guest,"fs",pocket_fs_install,NULL));
    TRY(pocketjs_guest_quickjs_install_once(guest,"imu",pocket_imu_install,NULL));
    TRY(pocketjs_guest_quickjs_install_once(guest,"av",pocket_av_install,NULL));
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
    const char *source=user_source?user_source:hello_start;
    TRY(pocketjs_guest_quickjs_install_once(guest,"pet-hub",pet_hub_install,NULL));
    TRY(pocketjs_guest_quickjs_install_once(guest,"pet-assets",pet_assets_install,core));
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
    pocketjs_rgb565_renderer_config_t rc;
    pocketjs_rgb565_renderer_config_defaults(&rc);rc.scale=1;
    TRY(pocketjs_rgb565_renderer_create(&rc,&renderer));
    TRY(pocketjs_rgb565_target_create(&target));
    app_report();
    return ESP_OK;
fail:
    ESP_LOGE("app","START_FAILED %s",esp_err_to_name(err));
    app_stop();return err;
}
esp_err_t app_start(void) { user_source=NULL; user_prelude=NULL; return app_start_test(0); }

esp_err_t app_start_source(const char *prelude, size_t prelude_length,
                           const char *source, size_t length) {
    user_source=source; user_length=length;
    user_prelude=prelude; user_prelude_length=prelude_length;
    esp_err_t err=app_start_test(0);
    user_source=NULL; user_prelude=NULL;
    return err;
}

const char *app_error(void) {
    const char *e=jsconsole_error();
    return e?e:"";
}
esp_err_t app_tick(uint32_t buttons) {
    deadline=esp_timer_get_time()+250000;
    // Watch deliveries before the frame, so a listener that updates a node and
    // the frame that draws it are the same turn rather than one apart.
    // First: it posts the sleeps that came due, so pocket_api_pump() settles
    // them this turn, and it is where Starting becomes Running.
    pocket_app_pump();
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
    pocket_api_pump();
    pocket_av_pump();
    // The same mask the turn below is handed: pocket.input reports what the
    // host forwarded, never a second reading of the keyboard.
    pocket_ui_pump(buttons);
    pocketjs_ui_input_t input={.struct_size=sizeof(input),.buttons=buttons};
    pocketjs_ui_frame_view_t frame={.struct_size=sizeof(frame)};
    // The JS side of the frame: frame() in QuickJS plus the UI core's tick and
    // draw. Timed on every tick, painted or not, so turn_ms is its own number
    // next to render_ms rather than hidden inside the frame period.
    int64_t turning=esp_timer_get_time();
    esp_err_t e=pocketjs_ui_turn(binding,&input,&frame);
    turn_sum+=(double)(esp_timer_get_time()-turning); ticks++;
    if(e)return e;
    pocketjs_rgb565_damage_plan_t plan={.struct_size=sizeof(plan)};
    e=pocketjs_rgb565_prepare(renderer,target,&frame,&plan);if(e)return e;
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
            e=pocketjs_rgb565_render_strip(renderer,&frame,pixels,LCD_W*rows,region,
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
    e=pocketjs_rgb565_commit(renderer,target,&frame);
    frames++;
    if(frames==1)ESP_LOGI("app","HELLO_FRAME_PRESENTED");
    return e;
fail:
    pocketjs_rgb565_abort(renderer,target);return e;
}
