#include "sdkconfig.h"
#ifdef CONFIG_DS_DEVICE_PROBE
#include "ds_core.h"
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
    ds_core_init(&probe_core);
    ds_client app=ds_core_client(&probe_core,DS_APP),system=ds_core_client(&probe_core,DS_SYSTEM);
    ds_tx tx;ds_ref moving,banner;
    ds_draw d={.kind=DS_RECT,.bounds={16,40,80,96},.clip={0,0,240,135},.opacity=255};
    d.data.shape.color=0x67dfc7ff;
    if(app.ops->begin(app.ctx,DS_REPLACE,&tx)!=DS_OK||
       app.ops->background(app.ctx,tx,0x0b1727ff)!=DS_OK||
       app.ops->add(app.ctx,tx,&d,&moving)!=DS_OK||app.ops->end(app.ctx,tx)!=DS_OK)goto fail;
    int64_t started=esp_timer_get_time();
    if(probe_display(&probe_core)!=DS_OK)goto fail;
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
        ds_change change={.property=DS_SET_RECT,.value.rect={160,40,224,96}};
        if(i&1)change.value.rect=(ds_rect){16,40,80,96};
        if(app.ops->begin(app.ctx,DS_PATCH,&tx)!=DS_OK||
           app.ops->change(app.ctx,tx,moving,&change)!=DS_OK||app.ops->end(app.ctx,tx)!=DS_OK)goto fail;
        ds_frame frame;
        if(ds_core_frame(&probe_core,&frame)!=DS_OK||ds_core_discard(&probe_core,frame.ticket)!=DS_OK)goto fail;
        int64_t elapsed=esp_timer_get_time()-started;sum+=elapsed;if(elapsed>max)max=elapsed;
        if(i%100==99)vTaskDelay(1);
    }
    size_t free_after=heap_caps_get_free_size(MALLOC_CAP_8BIT);
    ds_change change={.property=DS_SET_RECT,.value.rect={160,40,224,96}};
    if(app.ops->begin(app.ctx,DS_PATCH,&tx)!=DS_OK||app.ops->change(app.ctx,tx,moving,&change)!=DS_OK||
       app.ops->end(app.ctx,tx)!=DS_OK)goto fail;
    board_capture(true);
    ds_result display_result=probe_display(&probe_core);
    board_capture(false);
    if(display_result!=DS_OK)goto fail;
    ESP_LOGI(tag,"PASS iterations=1000 tx_mean_us=%lld tx_max_us=%lld heap_before=%u heap_after=%u stack_free=%u",
             (long long)(sum/1000),(long long)max,(unsigned)free_before,(unsigned)free_after,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    vTaskDelay(pdMS_TO_TICKS(5000));
    return;
fail:
    ESP_LOGE(tag,"FAIL display/transaction");
}
#endif
