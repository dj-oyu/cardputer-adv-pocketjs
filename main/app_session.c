#include "app_session.h"
#include "board.h"
#include "fonts.h"
#include "pocketjs/guest.h"
#include "pocketjs/guest_quickjs.h"
#include "pocketjs/ui_core.h"
#include "pocketjs/ui_qjs.h"
#include "pocketjs/render_rgb565.h"
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
static uint16_t pixels[LCD_W*STRIP_H];
static atomic_bool stop_requested;
static int64_t deadline;
static unsigned frames;
static bool redraw;
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
void app_report(void) {
    pocketjs_guest_stats_t stats={.struct_size=sizeof(stats)};
    if(guest) pocketjs_guest_stats(guest,&stats);
    ESP_LOGI("app","MEM free=%u largest=%u js=%u frames=%u",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
        (unsigned)stats.heap_used,frames);
}
void app_stop(void) {
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
    pocketjs_ui_core_config_t cc;
    pocketjs_ui_core_config_defaults(&cc);
    cc.logical_width=LCD_W;cc.logical_height=LCD_H;cc.raster_density=1;cc.tick_hz=30;
    TRY(pocketjs_ui_core_create(&cc,&core));
    TRY(pocketjs_ui_core_load_font_atlas(core,font_small,sizeof(font_small)));
    TRY(pocketjs_ui_core_load_font_atlas(core,font_large,sizeof(font_large)));
    pocketjs_ui_qjs_config_t bc={.struct_size=sizeof(bc),.target_id="cardputer-adv",.host_abi=1};
    TRY(pocketjs_ui_qjs_create(guest,core,&bc,&binding));
    TRY(pocketjs_ui_qjs_mount(binding));
    const char *source=hello_start;
    size_t length=hello_end-hello_start-1;
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
    TRY(pocketjs_guest_eval(guest,source,length,test?"diagnostic.js":"hello.js"));
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
esp_err_t app_start(void) { return app_start_test(0); }
esp_err_t app_tick(uint32_t buttons) {
    deadline=esp_timer_get_time()+250000;
    pocketjs_ui_input_t input={.struct_size=sizeof(input),.buttons=buttons};
    pocketjs_ui_frame_view_t frame={.struct_size=sizeof(frame)};
    esp_err_t e=pocketjs_ui_turn(binding,&input,&frame);
    if(e)return e;
    pocketjs_rgb565_damage_plan_t plan={.struct_size=sizeof(plan)};
    e=pocketjs_rgb565_prepare(renderer,target,&frame,&plan);if(e)return e;
    if(plan.region_count || redraw) {
        redraw=false;
        // Full-width strips avoid copying undefined columns of a narrow damage rect.
        for(int y=0;y<LCD_H;y+=STRIP_H) {
            int rows=LCD_H-y<STRIP_H?LCD_H-y:STRIP_H;
            memset(pixels,0,sizeof(pixels));
            pocketjs_rgb565_rect_t region={.x=0,.y=y,.width=LCD_W,.height=rows};
            pocketjs_rgb565_render_stats_t stats={.struct_size=sizeof(stats)};
            e=pocketjs_rgb565_render_strip(renderer,&frame,pixels,LCD_W*rows,region,NULL,&stats);
            if(e)goto fail;
            e=board_present(y,rows,pixels);if(e)goto fail;
        }
    }
    e=pocketjs_rgb565_commit(renderer,target,&frame);
    frames++;
    if(frames==1)ESP_LOGI("app","HELLO_FRAME_PRESENTED");
    return e;
fail:
    pocketjs_rgb565_abort(renderer,target);return e;
}
