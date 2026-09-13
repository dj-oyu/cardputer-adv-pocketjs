#include "sdkconfig.h"
#ifdef CONFIG_DS_DEVICE_PROBE
#include "ds_core.h"
#include "ds_cache.h"
#include "ds_render.h"
#include "board.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Run the same regression bodies on the target ABI, outside timed regions. */
#include "../../../tools/ds_contract/use_cases.c"
#define main ds_probe_core_tests
#include "../../../tools/ds_contract/test_core.c"
#undef main
#undef CHECK
#define main ds_probe_review_tests
#include "../../../tools/ds_contract/test_review.c"
#undef main

static ds_core probe_core;
static ds_cache probe_cache;
static uint16_t *probe_strip(void *ctx){(void)ctx;return board_strip();}
static ds_result probe_send(void *ctx,uint16_t y,uint16_t rows,const uint16_t *pixels){
    (void)ctx;
    if(pixels!=board_strip())return DS_INVALID;
    return board_present(y,rows,board_strip())==ESP_OK?DS_OK:DS_IO;
}

/* Diagnostic only: opaque rectangles and full-frame synchronous transfer.
 * This is deliberately not the production compositor or a damage benchmark. */
static ds_result probe_display(ds_core *core){
    ds_frame frame;
    ds_result result=ds_core_frame(core,&frame);
    if(result!=DS_OK)return result;
    uint16_t *pixels=board_strip();
    for(int y=0;y<LCD_H;y+=STRIP_H){
        int rows=LCD_H-y<STRIP_H?LCD_H-y:STRIP_H;
        uint32_t bg=frame.next_background;
        for(int i=0;i<LCD_W*rows;i++)pixels[i]=board_rgb(bg>>24,(bg>>16)&255,(bg>>8)&255);
        for(unsigned layer=0;layer<2;layer++)for(unsigned i=0;i<frame.next[layer].commands;i++){
            ds_frame_command command;
            result=ds_core_read(core,frame.ticket,false,(ds_layer)layer,i,&command);
            if(result!=DS_OK)return result;
            ds_draw *d=&command.draw;
            if(d->kind!=DS_RECT||d->opacity!=255||(d->data.shape.color&255)!=255)return DS_UNSUPPORTED;
            if(!command.visible)continue;
            uint32_t c=d->data.shape.color;
            uint16_t color=board_rgb(c>>24,(c>>16)&255,(c>>8)&255);
            for(int row=0;row<rows;row++)for(int x=0;x<LCD_W;x++){
                int py=y+row;
                if(x>=d->bounds.x0&&x<d->bounds.x1&&py>=d->bounds.y0&&py<d->bounds.y1&&
                   x>=d->clip.x0&&x<d->clip.x1&&py>=d->clip.y0&&py<d->clip.y1)
                    pixels[row*LCD_W+x]=color;
            }
        }
        if(board_present(y,rows,pixels)!=ESP_OK){
            ds_core_failed(core,frame.ticket);return DS_IO;
        }
    }
    return ds_core_presented(core,frame.ticket);
}

void ds_device_probe_run(void){
    const char *tag="DS_PROBE";
    failures=0;
    ESP_LOGI(tag,"START core=%u frame_command=%u",(unsigned)sizeof(ds_core),(unsigned)sizeof(ds_frame_command));
    int core_result=ds_probe_core_tests(),review_result=ds_probe_review_tests();
    if(core_result||review_result){ESP_LOGE(tag,"FAIL regression core=%d review=%d",core_result,review_result);return;}
    ds_core_init(&probe_core);ds_cache_init(&probe_cache);
    ds_client app=ds_core_client(&probe_core,DS_APP),system=ds_core_client(&probe_core,DS_SYSTEM);
    ds_tx tx;ds_ref banner;
    ds_draw d={.kind=DS_RECT,.bounds={0,0,64,56},.clip={0,0,64,56},.opacity=255};
    d.data.shape.color=0x67dfc7ff;
    ds_template block;ds_instance left_instance,right_instance;
    ds_placement left={16,40,{0,0,240,135},255,true};
    ds_placement right={160,40,{0,0,240,135},255,true};
    if(ds_cache_create(&probe_cache,DS_APP,&d,1,&block)!=DS_OK)goto fail;
    if(app.ops->begin(app.ctx,DS_REPLACE,&tx)!=DS_OK||
       app.ops->background(app.ctx,tx,0x0b1727ff)!=DS_OK||
       ds_cache_instantiate(&probe_cache,&probe_core,tx,block,&left,&left_instance)!=DS_OK||
       ds_cache_instantiate(&probe_cache,&probe_core,tx,block,&right,&right_instance)!=DS_OK||
       app.ops->end(app.ctx,tx)!=DS_OK)goto fail;
    ds_frame initial_frame;if(ds_core_frame(&probe_core,&initial_frame)!=DS_OK)goto fail;
    int64_t started=esp_timer_get_time();
    if(probe_display(&probe_core)!=DS_OK||
       ds_cache_resolve(&probe_cache,&probe_core,initial_frame.ticket,true)!=DS_OK)goto fail;
    ESP_LOGI(tag,"LCD initial_us=%lld bytes=64800",(long long)(esp_timer_get_time()-started));
    vTaskDelay(pdMS_TO_TICKS(1500));
    d.bounds=(ds_rect){0,0,240,24};d.clip=(ds_rect){8,8,232,135};d.data.shape.color=0xf5bb69ff;
    if(system.ops->begin(system.ctx,DS_REPLACE,&tx)!=DS_OK||
       system.ops->add(system.ctx,tx,&d,&banner)!=DS_OK||system.ops->end(system.ctx,tx)!=DS_OK||
       probe_display(&probe_core)!=DS_OK)goto fail;
    vTaskDelay(pdMS_TO_TICKS(1500));
    /* Measure transaction work without logging, display, or intentional waits. */
    size_t free_before=heap_caps_get_free_size(MALLOC_CAP_8BIT);
    int64_t sum=0,max=0;
    for(unsigned i=0;i<1000;i++){
        started=esp_timer_get_time();
        ds_placement trial=right;if(i&1)trial=left;
        if(app.ops->begin(app.ctx,DS_PATCH,&tx)!=DS_OK||
           ds_cache_place(&probe_cache,&probe_core,tx,right_instance,&trial)!=DS_OK||
           app.ops->end(app.ctx,tx)!=DS_OK)goto fail;
        ds_frame frame;
        if(ds_core_frame(&probe_core,&frame)!=DS_OK||ds_core_discard(&probe_core,frame.ticket)!=DS_OK||
           ds_cache_resolve(&probe_cache,&probe_core,frame.ticket,false)!=DS_OK)goto fail;
        int64_t elapsed=esp_timer_get_time()-started;sum+=elapsed;if(elapsed>max)max=elapsed;
        if(i%100==99)vTaskDelay(1);
    }
    size_t free_after=heap_caps_get_free_size(MALLOC_CAP_8BIT);
    if(app.ops->begin(app.ctx,DS_PATCH,&tx)!=DS_OK||
       ds_cache_set_visible(&probe_cache,&probe_core,tx,left_instance,false)!=DS_OK||
       ds_cache_place(&probe_cache,&probe_core,tx,right_instance,&right)!=DS_OK||
       app.ops->end(app.ctx,tx)!=DS_OK)goto fail;
    ds_frame partial_frame;if(ds_core_frame(&probe_core,&partial_frame)!=DS_OK)goto fail;
    ds_display_port display={NULL,probe_strip,probe_send,240,135,8};ds_render_stats rendered;
    started=esp_timer_get_time();
    if(ds_render_rects(&probe_core,&display,&rendered)!=DS_OK||rendered.bands!=0xfe0u||
       rendered.transferred_bytes!=26880||
       ds_cache_resolve(&probe_cache,&probe_core,partial_frame.ticket,true)!=DS_OK)goto fail;
    ESP_LOGI(tag,"PARTIAL us=%lld mask=%lx bytes=%lu",(long long)(esp_timer_get_time()-started),
             (unsigned long)rendered.bands,(unsigned long)rendered.transferred_bytes);
    if(app.ops->begin(app.ctx,DS_PATCH,&tx)!=DS_OK||app.ops->end(app.ctx,tx)!=DS_OK||
       ds_render_rects(&probe_core,&display,&rendered)!=DS_OK||rendered.bands||rendered.transferred_bytes)goto fail;
    ESP_LOGI(tag,"UNCHANGED bands=0 bytes=0");
    /* Capture all rows separately; capture traffic is outside timing. */
    if(app.ops->begin(app.ctx,DS_PATCH,&tx)!=DS_OK||app.ops->end(app.ctx,tx)!=DS_OK)goto fail;
    ds_frame capture_frame;
    if(ds_core_frame(&probe_core,&capture_frame)!=DS_OK||
       ds_core_failed(&probe_core,capture_frame.ticket)!=DS_OK)goto fail;
    board_capture(true);
    ds_result display_result=ds_render_rects(&probe_core,&display,&rendered);
    board_capture(false);
    if(display_result!=DS_OK||rendered.transferred_bytes!=64800)goto fail;
    ds_cache_stats cache_stats=ds_cache_get_stats(&probe_cache);
    ESP_LOGI(tag,"CACHE templates=%u instances=%u commands=%u native=%lu",
             cache_stats.templates,cache_stats.instances,cache_stats.commands,(unsigned long)cache_stats.native_bytes);
    ESP_LOGI(tag,"PASS iterations=1000 tx_mean_us=%lld tx_max_us=%lld heap_before=%u heap_after=%u stack_free=%u",
             (long long)(sum/1000),(long long)max,(unsigned)free_before,(unsigned)free_after,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    vTaskDelay(pdMS_TO_TICKS(5000));
    return;
fail:
    ESP_LOGE(tag,"FAIL display/transaction");
}
#endif
