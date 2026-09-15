#include "sdkconfig.h"
#ifdef CONFIG_KSN_DEVICE_PROBE
#include "ksn_core.h"
#include "ksn_cache.h"
#include "ksn_modal.h"
#include "ksn_frost.h"
#include "ksn_view_host.h"
#include "ksn_render.h"
#include "board.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Run the same regression bodies on the target ABI, outside timed regions. */
#include "../../../tools/kasane_contract/use_cases.c"
#define main ksn_probe_core_tests
#include "../../../tools/kasane_contract/test_core.c"
#undef main
#undef CHECK
#define main ksn_probe_review_tests
#include "../../../tools/kasane_contract/test_review.c"
#undef main

static ksn_core probe_core;
static ksn_core_command_block probe_commands[2];
static ksn_core_text_block probe_text[2];
static ksn_cache probe_cache;
static ksn_cache_command_block probe_cache_commands;
static ksn_cache_text_block probe_cache_text;
static ksn_frost probe_frost;
ksn_result ksn_stress_probe_run(ksn_frost *frost);
static uint16_t glass_pattern(int x,int y){
    x%=120;
    if(y<16)return board_rgb(13,23,39);
    if((x/12+y/12)%2)return board_rgb(30,160,220);
    return board_rgb(235,128,48);
}
static uint16_t glass_tint(uint16_t p){
    unsigned r=p>>11,g=(p>>5)&63,b=p&31;
    r=(r<<3)|(r>>2);g=(g<<2)|(g>>4);b=(b<<3)|(b>>2);
    return board_rgb((28*96+r*159+127)/255,(48*96+g*159+127)/255,(67*96+b*159+127)/255);
}
static ksn_result glass_show(bool blurred,bool capture){
    board_capture(capture);
    for(int y=0;y<135;y+=8){
        int rows=y==128?7:8;uint16_t *pixels=board_strip();
        for(int row=0;row<rows;row++){
            int py=y+row;
            for(int x=0;x<240;x++)pixels[row*240+x]=glass_pattern(x,py);
            if(blurred&&py>=24&&py<119){
                ksn_result result=ksn_frost_span(&probe_frost,(uint16_t)py,124,112,0x1c304360,pixels+row*240+124);
                if(result!=KSN_OK){board_capture(false);return result;}
            }
            for(int x=0;x<240;x++){
                int local=x%120;uint16_t *p=&pixels[row*240+x];
                if(local>=4&&local<116&&py>=24&&py<119){
                    if(x<120||!blurred)*p=glass_tint(*p);
                    /* Modal chrome stays sharp, outside the captured source. */
                    if(local==4||local==115||py==24||py==118)*p=board_rgb(173,206,225);
                    if(local>=20&&local<92&&py>=42&&py<46)*p=board_rgb(238,244,250);
                    if(local>=20&&local<76&&py>=54&&py<57)*p=board_rgb(173,206,225);
                    if(local>=20&&local<100&&py>=85&&py<105)*p=board_rgb(71,199,174);
                }
                /* Left: one marker; right: two markers. */
                if(py>=5&&py<11&&((local>=8&&local<14)||(x>=120&&local>=19&&local<25)))
                    *p=board_rgb(238,244,250);
            }
        }
        if(board_present_sync(y,rows,pixels)!=ESP_OK){board_capture(false);return KSN_IO;}
    }
    board_capture(false);return KSN_OK;
}
static ksn_result glass_demo(void){
    const char *tag="KSN_PROBE";
    ESP_LOGI(tag,"GLASS left=alpha right=frost start");
    if(glass_show(false,false)!=KSN_OK)return KSN_IO;
    vTaskDelay(pdMS_TO_TICKS(3000));
    for(unsigned radius=1;radius<=2;radius++){
        ksn_frost_init(&probe_frost);
        int64_t started=esp_timer_get_time();
        for(unsigned y=0;y<135;y+=8){
            unsigned rows=y==128?7:8;uint16_t *pixels=board_strip();
            for(unsigned row=0;row<rows;row++)for(unsigned x=0;x<240;x++)pixels[row*240+x]=glass_pattern(x,y+row);
            ksn_result result=ksn_frost_feed(&probe_frost,y,rows,pixels);if(result!=KSN_OK)return result;
        }
        ksn_result result=ksn_frost_blur(&probe_frost,(uint8_t)radius);if(result!=KSN_OK)return result;
        int64_t prepare_us=esp_timer_get_time()-started;
        started=esp_timer_get_time();result=glass_show(true,false);if(result!=KSN_OK)return result;
        ESP_LOGI(tag,"GLASS radius=%u prepare_us=%lld display_us=%lld bytes=%u",radius,
                 (long long)prepare_us,(long long)(esp_timer_get_time()-started),(unsigned)sizeof(probe_frost));
        if(radius==2){
            ESP_LOGI(tag,"GLASS_PIX_BEGIN");result=glass_show(true,true);
            ESP_LOGI(tag,"GLASS_PIX_END");if(result!=KSN_OK)return result;
        }
        vTaskDelay(pdMS_TO_TICKS(radius==1?4000:12000));
    }
    ESP_LOGI(tag,"GLASS PASS");return KSN_OK;
}
static uint16_t *probe_strip(void *ctx){(void)ctx;return board_strip();}
static ksn_result probe_send(void *ctx,uint16_t y,uint16_t rows,const uint16_t *pixels){
    (void)ctx;
    if(pixels!=board_strip())return KSN_INVALID;
    return board_present_sync(y,rows,board_strip())==ESP_OK?KSN_OK:KSN_IO;
}

/* Use the production compositor in diagnostics, including initial frames. */
static ksn_result probe_display(ksn_core *core){
    ksn_display_port display={NULL,probe_strip,probe_send,240,135,8};
    ksn_render_stats stats;return ksn_render_rects(core,&display,&stats);
}

static ksn_result view_demo(void){
    ksn_cache_bind(&probe_cache,&probe_cache_commands,&probe_cache_text);
    ksn_core_bind(&probe_core,&probe_commands[0],&probe_commands[1],&probe_text[0],&probe_text[1]);
    ksn_view_host host;ksn_view_host_init(&host,&probe_core,&probe_cache,42);
    ksn_view *app=ksn_view_host_endpoint(&host,KSN_APP);
    ksn_display_port display={NULL,probe_strip,probe_send,240,135,8};
    ksn_render_stats stats;ksn_tx tx;ksn_template shape;ksn_instance a,b;
    ksn_draw d={.kind=KSN_RECT,.bounds={0,0,64,48},.clip={0,0,240,135},
               .opacity=255,.data.shape={0x67dfc7ff,0,0}};
    ksn_placement p={16,40,{0,0,240,135},128,true};
#define V(call) do{ksn_result result=(call);if(result!=KSN_OK)return result;}while(0)
    V(ksn_view_cache_create(app,&d,1,&shape));
    V(ksn_view_begin(app,KSN_REPLACE,&tx));V(ksn_view_background(app,tx,0x0b1727ff));
    V(ksn_view_instantiate(app,tx,shape,&p,&a));p.x=104;
    V(ksn_view_instantiate(app,tx,shape,&p,&b));V(ksn_view_submit(app,tx));
    V(ksn_view_host_present(&host,&display,&stats));
    V(ksn_view_begin(app,KSN_PATCH,&tx));V(ksn_view_visible(app,tx,a,false));
    ksn_view_host_end_turn(&host); /* Simulated guest yield: visibility rolls back. */
    V(ksn_view_begin(app,KSN_PATCH,&tx));V(ksn_view_submit(app,tx));
    V(ksn_view_host_present(&host,&display,&stats));if(stats.bands)return KSN_INVALID;
    V(ksn_view_begin(app,KSN_REPLACE,&tx));
    V(ksn_view_modal_open(app,tx,KSN_MODAL_SOLID,0x1c3043ff,7));
    V(ksn_view_submit(app,tx));V(ksn_view_host_present(&host,&display,&stats));
    if(ksn_view_host_route(&host,false)!=KSN_INPUT_MODAL)return KSN_INVALID;
    V(ksn_view_cache_release(app,shape)); /* Omitted instances detached by host. */
    V(ksn_view_begin(app,KSN_REPLACE,&tx));V(ksn_view_modal_close(app,tx));
    V(ksn_view_background(app,tx,0x0b1727ff));V(ksn_view_submit(app,tx));
    V(ksn_view_host_present(&host,&display,&stats));
    if(ksn_view_host_route(&host,false)!=KSN_INPUT_APP||host.modal.focus!=42)return KSN_INVALID;
    ESP_LOGI("KSN_PROBE","VIEW PASS coordinator=%u cache=two-instances abort=yield modal=open-close",(unsigned)sizeof(host));
#undef V
    return KSN_OK;
}

void ksn_device_probe_run(void){
    const char *tag="KSN_PROBE";
    failures=0;
    ESP_LOGI(tag,"START core=%u frame_command=%u",(unsigned)KSN_CORE_RESERVED_BYTES,(unsigned)sizeof(ksn_frame_command));
    int core_result=ksn_probe_core_tests(),review_result=ksn_probe_review_tests();
    if(core_result||review_result){ESP_LOGE(tag,"FAIL regression core=%d review=%d",core_result,review_result);return;}
    ksn_core_bind(&probe_core,&probe_commands[0],&probe_commands[1],&probe_text[0],&probe_text[1]);
    ksn_cache_bind(&probe_cache,&probe_cache_commands,&probe_cache_text);
    ksn_client app=ksn_core_client(&probe_core,KSN_APP),system=ksn_core_client(&probe_core,KSN_SYSTEM);
    ksn_tx tx;ksn_ref banner;
    ksn_draw d={.kind=KSN_RECT,.bounds={0,0,64,56},.clip={0,0,64,56},.opacity=255};
    d.data.shape.color=0x67dfc7ff;
    ksn_template block;ksn_instance left_instance,right_instance;
    ksn_placement left={16,40,{0,0,240,135},255,true};
    ksn_placement right={160,40,{0,0,240,135},255,true};
    ksn_draw children[2]={d,d};children[1].bounds=(ksn_rect){16,16,48,40};
    children[1].data.shape.color=0xf5bb6980;
    if(ksn_cache_create(&probe_cache,KSN_APP,children,2,&block)!=KSN_OK)goto fail;
    if(app.ops->begin(app.ctx,KSN_REPLACE,&tx)!=KSN_OK||
       app.ops->background(app.ctx,tx,0x0b1727ff)!=KSN_OK||
       ksn_cache_instantiate(&probe_cache,&probe_core,tx,block,&left,&left_instance)!=KSN_OK||
       ksn_cache_instantiate(&probe_cache,&probe_core,tx,block,&right,&right_instance)!=KSN_OK||
       app.ops->end(app.ctx,tx)!=KSN_OK)goto fail;
    ksn_frame initial_frame;if(ksn_core_frame(&probe_core,&initial_frame)!=KSN_OK)goto fail;
    int64_t started=esp_timer_get_time();
    if(probe_display(&probe_core)!=KSN_OK||
       ksn_cache_resolve(&probe_cache,&probe_core,initial_frame.ticket,true)!=KSN_OK)goto fail;
    ESP_LOGI(tag,"LCD initial_us=%lld bytes=64800",(long long)(esp_timer_get_time()-started));
    vTaskDelay(pdMS_TO_TICKS(1500));
    d.bounds=(ksn_rect){0,0,240,24};d.clip=(ksn_rect){8,8,232,135};d.data.shape.color=0xf5bb69ff;
    if(system.ops->begin(system.ctx,KSN_REPLACE,&tx)!=KSN_OK||
       system.ops->add(system.ctx,tx,&d,&banner)!=KSN_OK||system.ops->end(system.ctx,tx)!=KSN_OK||
       probe_display(&probe_core)!=KSN_OK)goto fail;
    vTaskDelay(pdMS_TO_TICKS(1500));
    /* Measure transaction work without logging, display, or intentional waits. */
    size_t free_before=heap_caps_get_free_size(MALLOC_CAP_8BIT);
    int64_t sum=0,max=0;
    for(unsigned i=0;i<1000;i++){
        started=esp_timer_get_time();
        ksn_placement trial=right;if(i&1)trial=left;
        if(app.ops->begin(app.ctx,KSN_PATCH,&tx)!=KSN_OK||
           ksn_cache_place(&probe_cache,&probe_core,tx,right_instance,&trial)!=KSN_OK||
           app.ops->end(app.ctx,tx)!=KSN_OK)goto fail;
        ksn_frame frame;
        if(ksn_core_frame(&probe_core,&frame)!=KSN_OK||ksn_core_discard(&probe_core,frame.ticket)!=KSN_OK||
           ksn_cache_resolve(&probe_cache,&probe_core,frame.ticket,false)!=KSN_OK)goto fail;
        int64_t elapsed=esp_timer_get_time()-started;sum+=elapsed;if(elapsed>max)max=elapsed;
        if(i%100==99)vTaskDelay(1);
    }
    size_t free_after=heap_caps_get_free_size(MALLOC_CAP_8BIT);
    right.opacity=128;
    if(app.ops->begin(app.ctx,KSN_PATCH,&tx)!=KSN_OK||
       ksn_cache_set_visible(&probe_cache,&probe_core,tx,left_instance,false)!=KSN_OK||
       ksn_cache_place(&probe_cache,&probe_core,tx,right_instance,&right)!=KSN_OK||
       app.ops->end(app.ctx,tx)!=KSN_OK)goto fail;
    ksn_frame partial_frame;if(ksn_core_frame(&probe_core,&partial_frame)!=KSN_OK)goto fail;
    ksn_display_port display={NULL,probe_strip,probe_send,240,135,8};ksn_render_stats rendered;
    started=esp_timer_get_time();
    if(ksn_render_rects(&probe_core,&display,&rendered)!=KSN_OK||rendered.bands!=0xfe0u||
       rendered.transferred_bytes!=26880||
       ksn_cache_resolve(&probe_cache,&probe_core,partial_frame.ticket,true)!=KSN_OK)goto fail;
    ESP_LOGI(tag,"PARTIAL us=%lld mask=%lx bytes=%lu",(long long)(esp_timer_get_time()-started),
             (unsigned long)rendered.bands,(unsigned long)rendered.transferred_bytes);
    if(app.ops->begin(app.ctx,KSN_PATCH,&tx)!=KSN_OK||app.ops->end(app.ctx,tx)!=KSN_OK||
       ksn_render_rects(&probe_core,&display,&rendered)!=KSN_OK||rendered.bands||rendered.transferred_bytes)goto fail;
    ESP_LOGI(tag,"UNCHANGED bands=0 bytes=0");
    /* Capture all rows separately; capture traffic is outside timing. */
    if(app.ops->begin(app.ctx,KSN_PATCH,&tx)!=KSN_OK||app.ops->end(app.ctx,tx)!=KSN_OK)goto fail;
    ksn_frame capture_frame;
    if(ksn_core_frame(&probe_core,&capture_frame)!=KSN_OK||
       ksn_core_failed(&probe_core,capture_frame.ticket)!=KSN_OK)goto fail;
    board_capture(true);
    ksn_result display_result=ksn_render_rects(&probe_core,&display,&rendered);
    board_capture(false);
    if(display_result!=KSN_OK||rendered.transferred_bytes!=64800)goto fail;
    ksn_cache_stats cache_stats=ksn_cache_get_stats(&probe_cache);
    ESP_LOGI(tag,"CACHE templates=%u instances=%u commands=%u native=%lu",
             cache_stats.templates,cache_stats.instances,cache_stats.commands,(unsigned long)cache_stats.native_bytes);
    /* Exercise actual modal result/route transitions on the owner task. */
    ksn_modal modal;ksn_modal_init(&modal,42);
    if(app.ops->begin(app.ctx,KSN_REPLACE,&tx)!=KSN_OK||
       app.ops->background(app.ctx,tx,0x0b1727ff)!=KSN_OK||
       ksn_modal_prepare_open(&modal,&probe_core,tx,KSN_MODAL_DIM_LIVE,0x00000080,7)!=KSN_OK||
       ksn_modal_route(&modal,&probe_core,false)!=KSN_INPUT_BLOCKED)goto fail;
    d.bounds=(ksn_rect){40,32,200,112};d.clip=(ksn_rect){0,0,240,135};d.data.shape.color=0x1c3043ff;
    ksn_ref modal_panel;
    if(app.ops->add(app.ctx,tx,&d,&modal_panel)!=KSN_OK||app.ops->end(app.ctx,tx)!=KSN_OK||
       probe_display(&probe_core)!=KSN_OK||ksn_modal_resolve(&modal,&probe_core)!=KSN_OK||
       ksn_cache_resolve(&probe_cache,&probe_core,tx,true)!=KSN_OK||
       ksn_modal_route(&modal,&probe_core,false)!=KSN_INPUT_MODAL)goto fail;
    vTaskDelay(pdMS_TO_TICKS(1000));
    if(app.ops->begin(app.ctx,KSN_REPLACE,&tx)!=KSN_OK||
       ksn_modal_prepare_close(&modal,&probe_core,tx)!=KSN_OK||
       app.ops->background(app.ctx,tx,0x0b1727ff)!=KSN_OK||app.ops->end(app.ctx,tx)!=KSN_OK||
       probe_display(&probe_core)!=KSN_OK||ksn_modal_resolve(&modal,&probe_core)!=KSN_OK||
       ksn_modal_route(&modal,&probe_core,false)!=KSN_INPUT_APP||modal.focus!=42)goto fail;
    ESP_LOGI(tag,"COMPOSITION group_alpha=128 modal=open-close focus=42 PASS");
    if(view_demo()!=KSN_OK)goto fail;
    if(glass_demo()!=KSN_OK)goto fail;
    if(ksn_stress_probe_run(&probe_frost)!=KSN_OK)goto fail;
    ESP_LOGI(tag,"PASS iterations=1000 tx_mean_us=%lld tx_max_us=%lld heap_before=%u heap_after=%u stack_free=%u",
             (long long)(sum/1000),(long long)max,(unsigned)free_before,(unsigned)free_after,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    vTaskDelay(pdMS_TO_TICKS(5000));
    return;
fail:
    ESP_LOGE(tag,"FAIL display/transaction");
}
#endif
