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
#include "pocket_imu.h"
#include "pocket_av.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdatomic.h>
#include <string.h>

extern const char hello_start[] asm("_binary_main_js_start");
extern const char hello_end[] asm("_binary_main_js_end");
static pocketjs_guest_t *guest;
static pocketjs_ui_core_t *core;
static pocketjs_ui_qjs_t *binding;
static pocketjs_rgb565_renderer_t *renderer;
static pocketjs_rgb565_target_t *target;
static atomic_bool stop_requested;
static int64_t deadline;
static unsigned frames;
static bool redraw;
static double render_sum, present_sum, kernel_sum;
static unsigned painted;
// Hand-written PIE kernels for the two ops this renderer actually asks for
// (opaque fill, coverage-mask blend); anything they cannot honour exactly is
// declined and the Rust software path draws it.
extern const pocketjs_rgb565_accelerator_t render_accel;
extern uint32_t render_accel_cycles;
// Borrowed for the length of a start; the Playground owns the bytes and does
// not edit them while a run is up.
static const char *user_source;
static size_t user_length;
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

static esp_err_t eval_user_source(const char *source, size_t length) {
    JSContext *ctx=pocketjs_guest_quickjs_context(guest);
    if(!ctx) return ESP_ERR_INVALID_STATE;

    JSValue result=JS_Eval(ctx,source,length,"user.js",JS_EVAL_TYPE_GLOBAL);
    if(JS_IsException(result)) {
        JSValue exception=JS_GetException(ctx);
        char message[128]={0};
        const char *text=JS_ToCString(ctx,exception);
        if(text) { snprintf(message,sizeof(message),"%s",text); JS_FreeCString(ctx,text); }
        JSValue stack=JS_GetPropertyStr(ctx,exception,"stack");
        // A stack getter can itself throw. Clearing the new pending exception
        // here keeps the bind-frame evaluation below from inheriting it.
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
    // Before the guest goes: the watches hold callbacks belonging to it.
    pocket_imu_reset();
    pocket_av_reset();
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
    gc.heap_limit=128*1024; gc.stack_limit=20*1024; gc.prefer_psram=false;
#define TRY(expr) do {err=(expr);if(err!=ESP_OK)goto fail;}while(0)
    TRY(pocketjs_guest_create(&gc,&guest));
    TRY(pocketjs_guest_quickjs_install(guest,install_limits,NULL));
    // Replaces quickjs-libc's print, whose output only ever reaches stdout.
    jsconsole_clear();
    TRY(pocketjs_guest_quickjs_install_once(guest,"console",jsconsole_install,NULL));
    TRY(pocketjs_guest_quickjs_install_once(guest,"jsfont",jsfont_install,NULL));
    TRY(pocketjs_guest_quickjs_install_once(guest,"pocket",pocket_api_install,NULL));
    TRY(pocketjs_guest_quickjs_install_once(guest,"storage",pocket_storage_install,NULL));
    TRY(pocketjs_guest_quickjs_install_once(guest,"imu",pocket_imu_install,NULL));
    TRY(pocketjs_guest_quickjs_install_once(guest,"av",pocket_av_install,NULL));
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
        }
    }
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
esp_err_t app_start(void) { user_source=NULL; return app_start_test(0); }

esp_err_t app_start_source(const char *source, size_t length) {
    user_source=source; user_length=length;
    esp_err_t err=app_start_test(0);
    user_source=NULL;
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
    pocket_imu_pump();
    pocket_av_pump();
    pocketjs_ui_input_t input={.struct_size=sizeof(input),.buttons=buttons};
    pocketjs_ui_frame_view_t frame={.struct_size=sizeof(frame)};
    esp_err_t e=pocketjs_ui_turn(binding,&input,&frame);
    if(e)return e;
    pocketjs_rgb565_damage_plan_t plan={.struct_size=sizeof(plan)};
    e=pocketjs_rgb565_prepare(renderer,target,&frame,&plan);if(e)return e;
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
            ESP_LOGI("app","PAINT render_ms=%.2f kernel_ms=%.2f send_ms=%.2f accel=%u software=%u",
                     render_sum/30/1000.0, kernel_sum/30/240000.0, present_sum/30/1000.0,
                     (unsigned)accel,(unsigned)sw_ops);
            render_sum=0; present_sum=0; kernel_sum=0; painted=0;
        }
    }
    e=pocketjs_rgb565_commit(renderer,target,&frame);
    frames++;
    if(frames==1)ESP_LOGI("app","HELLO_FRAME_PRESENTED");
    return e;
fail:
    pocketjs_rgb565_abort(renderer,target);return e;
}
