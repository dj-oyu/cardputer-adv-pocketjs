#include "sdkconfig.h"
#ifdef CONFIG_KSN_DEVICE_PROBE
#include "ksn_frost.h"
#include "ksn_stress_scene.h"
#include "board.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static ksn_result verify_pie(ksn_frost *frost){
    uint16_t scalar[256],vector[256];uint32_t seed=19;
    ksn_frost_init(frost);
    for(unsigned i=0;i<510;i++){
        seed=seed*1664525u+1013904223u;frost->state.image[i]=(uint16_t)(seed>>8);
    }
    frost->state.phase=1;
    /* All alpha, every row, arbitrary span starts, all output alignments,
     * guards before/after the destination. The actual assembly runs here. */
    for(unsigned alpha=0;alpha<256;alpha++){
        unsigned x=alpha%241,count=240-x,offset=1+(alpha&7);
        ksn_rgba tint=0x1c304300|alpha;
        for(unsigned y=0;y<135;y++){
            for(unsigned i=0;i<256;i++)scalar[i]=vector[i]=0xdead;
            if(ksn_frost_span_scalar(frost,y,x,count,tint,scalar+offset)!=KSN_OK||
               ksn_frost_span(frost,y,x,count,tint,vector+offset)!=KSN_OK||
               memcmp(scalar,vector,sizeof(scalar)))return KSN_INVALID;
        }
        if(alpha%16==15)vTaskDelay(1);
    }
    /* Same binary, same snapshot, same arguments. Alternate measurement order
     * so instruction-cache/interrupt effects don't always favor one path. */
    uint64_t scalar_us=0,pie_us=0;unsigned checksum=0;
    for(unsigned trial=0;trial<64;trial++){
        unsigned x=24+trial%64;ksn_rgba tint=0x1c304300|(trial*4);
        for(unsigned pass=0;pass<2;pass++){
            bool pie=(pass^(trial&1))!=0;
            int64_t start=esp_timer_get_time();
            for(unsigned y=20;y<100;y++){
                if(pie)ksn_frost_span(frost,y,x,112,tint,vector+1);
                else ksn_frost_span_scalar(frost,y,x,112,tint,scalar+1);
            }
            uint64_t elapsed=(uint64_t)(esp_timer_get_time()-start);
            if(pie)pie_us+=elapsed;else scalar_us+=elapsed;
            checksum+=pie?vector[5]:scalar[5];
        }
        vTaskDelay(1);
    }
    ESP_LOGI("KSN_PROBE","PIE_AB PASS scalar_us=%llu pie_us=%llu trials=64 pixels_per_trial=8960 checksum=%u",
             (unsigned long long)scalar_us,(unsigned long long)pie_us,checksum);
    return KSN_OK;
}

/* Diagnostic-only counters; capture replay must not enter these totals. */
static uint64_t source_us,feed_us,blur_us,span_us,present_us;
#define STRESS_FRAMES 600u
#define FRAME_US 33333u
static ksn_result prepare(ksn_frost *frost,unsigned t,unsigned radius){
    stress_frame scene=stress_frame_prepare(t);
    ksn_frost_init(frost);
    for(unsigned y=0;y<135;y+=8){
        unsigned rows=y==128?7:8;uint16_t *pixels=board_strip();
        int64_t clock=esp_timer_get_time();
        for(unsigned row=0;row<rows;row++)stress_source_row(&scene,y+row,pixels+row*240);
        source_us+=esp_timer_get_time()-clock;clock=esp_timer_get_time();
        ksn_result result=ksn_frost_feed(frost,y,rows,pixels);if(result!=KSN_OK)return result;
        feed_us+=esp_timer_get_time()-clock;
    }
    int64_t clock=esp_timer_get_time();
    ksn_result result=ksn_frost_blur(frost,(uint8_t)radius);
    blur_us+=esp_timer_get_time()-clock;return result;
}
static ksn_result show(const ksn_frost *frost,unsigned t,bool capture){
    stress_frame scene=stress_frame_prepare(t);
    unsigned px,py,alpha;stress_panel(t,&px,&py,&alpha);
    board_capture(capture);
    for(unsigned y=0;y<135;y+=8){
        unsigned rows=y==128?7:8;uint16_t *pixels=board_strip();
        for(unsigned row=0;row<rows;row++){
            unsigned sy=y+row;
            int64_t clock=esp_timer_get_time();
            stress_source_row(&scene,sy,pixels+row*240);
            if(!capture)source_us+=esp_timer_get_time()-clock;
            if(sy>=py&&sy<py+80){
                clock=esp_timer_get_time();
                ksn_result result=ksn_frost_span(frost,sy,px,112,0x1c304300|alpha,pixels+row*240+px);
                if(!capture)span_us+=esp_timer_get_time()-clock;
                if(result!=KSN_OK){board_capture(false);return result;}
                for(unsigned x=px;x<px+112;x++){
                    if(x==px||x==px+111||sy==py||sy==py+79)pixels[row*240+x]=stress_rgb(173,206,225);
                    if(x>=px+16&&x<px+96&&sy>=py+18&&sy<py+22)pixels[row*240+x]=stress_rgb(238,244,250);
                    if(x>=px+16&&x<px+88&&sy>=py+52&&sy<py+68)pixels[row*240+x]=stress_rgb(71,199,174);
                }
            }
        }
        int64_t clock=esp_timer_get_time();
        if(board_present(y,rows,pixels)!=ESP_OK){board_capture(false);return KSN_IO;}
        if(!capture)present_us+=esp_timer_get_time()-clock;
    }
    board_capture(false);return KSN_OK;
}
ksn_result ksn_stress_probe_run(ksn_frost *frost){
    ksn_result verified=verify_pie(frost);if(verified!=KSN_OK)return verified;
    source_us=feed_us=blur_us=span_us=present_us=0;
    uint32_t samples[STRESS_FRAMES]; /* 2400 B diagnostic-only timing storage. */
    uint64_t work=0,prepare_total=0,show_total=0,capture_us=0;
    uint32_t misses=0,skipped=0;
    size_t heap_before=heap_caps_get_free_size(MALLOC_CAP_8BIT),heap_min=heap_before;
    size_t largest_before=heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),largest_min=largest_before;
    int64_t start=esp_timer_get_time(),next=start;
    ESP_LOGI("KSN_PROBE","STRESS start frames=600 target_us=33333 recapture=every-frame");
    for(unsigned frame=0;frame<STRESS_FRAMES;frame++){
        int64_t began=esp_timer_get_time();
        unsigned t=(unsigned)((began-start-capture_us)/1000),radius=1+(frame/60)%2;
        ksn_result result=prepare(frost,t,radius);if(result!=KSN_OK)return result;
        int64_t prepared=esp_timer_get_time();
        result=show(frost,t,false);if(result!=KSN_OK)return result;
        int64_t ended=esp_timer_get_time();
        samples[frame]=(uint32_t)(ended-began);work+=samples[frame];
        prepare_total+=(uint64_t)(prepared-began);show_total+=(uint64_t)(ended-prepared);
        next+=FRAME_US;
        if(ended>next)misses++;
        while(next<ended){next+=FRAME_US;skipped++;}
        if(frame==0||frame==299||frame==599){
            int64_t capture_start=esp_timer_get_time();
            ESP_LOGI("KSN_PROBE","STRESS_PIX_BEGIN frame=%u tick=%u radius=%u",frame,t,radius);
            result=show(frost,t,true);
            ESP_LOGI("KSN_PROBE","STRESS_PIX_END");if(result!=KSN_OK)return result;
            int64_t extra=esp_timer_get_time()-capture_start;capture_us+=(uint64_t)extra;next+=extra;
        }
        if(frame%60==59){
            size_t free_now=heap_caps_get_free_size(MALLOC_CAP_8BIT),largest=heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
            if(free_now<heap_min)heap_min=free_now;
            if(largest<largest_min)largest_min=largest;
            ESP_LOGI("KSN_PROBE","STRESS progress=%u frame_us=%lu heap=%u largest=%u",frame+1,
                     (unsigned long)samples[frame],(unsigned)free_now,(unsigned)largest);
        }
        if(frame+1<STRESS_FRAMES){
            int64_t remaining=next-esp_timer_get_time();
            TickType_t ticks=remaining>0?pdMS_TO_TICKS((uint32_t)(remaining/1000)):0;
            vTaskDelay(ticks?ticks:1);
        }
    }
    uint64_t elapsed=(uint64_t)(esp_timer_get_time()-start)-capture_us;
    size_t heap_after=heap_caps_get_free_size(MALLOC_CAP_8BIT),largest_after=heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    /* Sort outside measurement, without libc allocation or recursion. */
    for(unsigned i=1;i<STRESS_FRAMES;i++){
        uint32_t value=samples[i];unsigned j=i;
        while(j&&samples[j-1]>value){samples[j]=samples[j-1];j--;}
        samples[j]=value;
    }
    ESP_LOGI("KSN_PROBE","STRESS timing mean_us=%lu p95_us=%lu max_us=%lu prepare_mean_us=%lu show_mean_us=%lu",
             (unsigned long)(work/STRESS_FRAMES),(unsigned long)samples[569],(unsigned long)samples[599],
             (unsigned long)(prepare_total/STRESS_FRAMES),(unsigned long)(show_total/STRESS_FRAMES));
    ESP_LOGI("KSN_PROBE","STRESS split source_us=%llu feed_us=%llu blur_us=%llu span_us=%llu present_us=%llu",
        (unsigned long long)(source_us/STRESS_FRAMES),(unsigned long long)(feed_us/STRESS_FRAMES),
        (unsigned long long)(blur_us/STRESS_FRAMES),(unsigned long long)(span_us/STRESS_FRAMES),
        (unsigned long long)(present_us/STRESS_FRAMES));
    ESP_LOGI("KSN_PROBE","STRESS cadence elapsed_us=%llu capture_us=%llu misses=%lu skipped=%lu bytes=%lu",
             (unsigned long long)elapsed,(unsigned long long)capture_us,(unsigned long)misses,(unsigned long)skipped,
             (unsigned long)(STRESS_FRAMES*64800u));
    ESP_LOGI("KSN_PROBE","STRESS memory before=%u min=%u after=%u largest_before=%u largest_min=%u largest_after=%u stack_free=%u",
             (unsigned)heap_before,(unsigned)heap_min,(unsigned)heap_after,(unsigned)largest_before,(unsigned)largest_min,
             (unsigned)largest_after,(unsigned)uxTaskGetStackHighWaterMark(NULL));
    ESP_LOGI("KSN_PROBE","STRESS PASS frames=600");return KSN_OK;
}
#endif
