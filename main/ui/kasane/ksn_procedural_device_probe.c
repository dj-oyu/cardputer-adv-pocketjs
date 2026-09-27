#include "ksn_procedural_present.h"
#ifdef KASANE_PROC_DEVICE_PROBE
#include "ksn_proc_compiler_device_probe.h"
#include "board.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "KSN_PROC"
#define I(OP,D,A,B,V,C) {OP,D,A,B,V,C}

/* Ninety-five connected segments. One native run supplies the whole curve. */
static const ksn_proc_inst wave_code[]={
    I(KSN_PROC_SET,0,0,0,10,0), I(KSN_PROC_SET,1,0,0,2,0),
    I(KSN_PROC_INPUT,2,0,0,0,0), I(KSN_PROC_SET,3,0,0,22,0),
    I(KSN_PROC_SET,4,0,0,0.13f,0),
    I(KSN_PROC_REPEAT,0,96,0,0,0),
      I(KSN_PROC_MUL,5,0,4,0,0), I(KSN_PROC_SIN,6,5,0,0,0),
      I(KSN_PROC_MUL,6,6,3,0,0), I(KSN_PROC_ADD,7,6,2,0,0),
      I(KSN_PROC_LINE,0,0,7,0,0xf800),
      I(KSN_PROC_ADD,0,0,1,0,0),
    I(KSN_PROC_END,0,0,0,0,0)
};
static const ksn_proc_inst grid_code[]={
    I(KSN_PROC_SET,1,0,0,20,0), I(KSN_PROC_SET,2,0,0,19,0),
    I(KSN_PROC_SET,3,0,0,18,0),
    I(KSN_PROC_REPEAT,0,6,0,0,0),
      I(KSN_PROC_SET,0,0,0,25,0),
      I(KSN_PROC_REPEAT,0,10,0,0,0),
        I(KSN_PROC_PLOT,0,0,1,0,0x07ff),
        I(KSN_PROC_ADD,0,0,2,0,0),
      I(KSN_PROC_END,0,0,0,0,0),
      I(KSN_PROC_ADD,1,1,3,0,0),
    I(KSN_PROC_END,0,0,0,0,0)
};
static const ksn_proc_program wave={wave_code,sizeof wave_code/sizeof wave_code[0]};
static const ksn_proc_program grid={grid_code,sizeof grid_code/sizeof grid_code[0]};

typedef struct {
    ksn_core *core;
    ksn_core_command_block *commands[2];
    ksn_core_text_block *text[2];
    ksn_proc_surface *surface[2];
    ksn_proc_frame *frame;
    ksn_proc_vm *vm;
} probe_storage;
static void *internal_alloc(size_t bytes){
    return heap_caps_calloc(1,bytes,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
}
static void release(probe_storage *s){
    heap_caps_free(s->vm);heap_caps_free(s->frame);
    heap_caps_free(s->surface[1]);heap_caps_free(s->surface[0]);
    heap_caps_free(s->text[1]);heap_caps_free(s->text[0]);
    heap_caps_free(s->commands[1]);heap_caps_free(s->commands[0]);
    heap_caps_free(s->core);
}
static bool backdrop(void *ctx,uint16_t *pixels,int y,int rows){
    (void)ctx;
    for(int row=0;row<rows;row++)for(int x=0;x<KSN_PROC_W;x++){
        int py=y+row;
        pixels[row*KSN_PROC_W+x]=((x/16+py/16)&1)?0x0842:0x1084;
    }
    return true;
}
typedef struct { int64_t send_us; } probe_port;
static uint16_t *strip(void *ctx){(void)ctx;return board_strip();}
static ksn_result present(void *ctx,uint16_t y,uint16_t rows,const uint16_t *pixels){
    probe_port *port=(probe_port *)ctx;
    if(pixels!=board_strip())return KSN_INVALID;
    int64_t started=esp_timer_get_time();
    esp_err_t result=board_present_sync(y,rows,board_strip());
    port->send_us+=esp_timer_get_time()-started;
    return result==ESP_OK?KSN_OK:KSN_IO;
}
static bool prepare(probe_storage *s,const ksn_proc_program *program,float baseline){
    const float input[KSN_PROC_INPUTS]={baseline,0,0,0};
    return ksn_proc_begin(s->vm,program,input,s->frame)==KSN_PROC_RUNNING&&
           ksn_proc_run(s->vm)==KSN_PROC_DONE;
}
void ksn_proc_device_probe_run(void){
    bool display_ok=false;
    probe_storage s={0};
    ksn_proc_layers layers;
    probe_port port={0};
    ksn_display_port display={.ctx=&port,.strip=strip,.present=present,
        .width=KSN_PROC_W,.height=KSN_PROC_H,.strip_rows=8};
    ksn_render_stats stats;
    size_t free_before=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    ESP_LOGI(TAG,"START free=%u largest=%u surface=%u frame=%u core=%u",
        (unsigned)free_before,
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
        (unsigned)sizeof(ksn_proc_surface),(unsigned)sizeof(ksn_proc_frame),
        (unsigned)KSN_CORE_RESERVED_BYTES);
    s.core=internal_alloc(sizeof *s.core);
    for(unsigned i=0;i<2;i++){
        s.commands[i]=internal_alloc(sizeof *s.commands[i]);
        s.text[i]=internal_alloc(sizeof *s.text[i]);
    }
    s.surface[0]=internal_alloc(sizeof *s.surface[0]);
    s.frame=internal_alloc(sizeof *s.frame);
    s.vm=internal_alloc(sizeof *s.vm);
    if(!s.core||!s.commands[0]||!s.commands[1]||!s.text[0]||!s.text[1]||
       !s.surface[0]||!s.frame||!s.vm){
        ESP_LOGE(TAG,"ALLOC_FAIL free=%u largest=%u",
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
        goto done;
    }
    if(ksn_core_bind(s.core,s.commands[0],s.commands[1],s.text[0],s.text[1])!=KSN_OK)
        goto fail;
    ksn_proc_surface_init(s.surface[0]);
    ksn_proc_layers_init(&layers,backdrop,0);
    if(!ksn_proc_layers_add(&layers,s.surface[0]))goto fail;
    s.surface[1]=internal_alloc(sizeof *s.surface[1]);
    if(s.surface[1]){
        ksn_proc_surface_init(s.surface[1]);
        if(!ksn_proc_layers_add(&layers,s.surface[1]))goto fail;
    }
    ESP_LOGI(TAG,"ALLOC surfaces=%u free=%u largest=%u min_free=%u stack_free=%u",
        (unsigned)layers.count,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
        (unsigned)uxTaskGetStackHighWaterMark(NULL));

    ksn_client system=ksn_core_client(s.core,KSN_SYSTEM);
    ksn_tx tx;ksn_ref ref;
    ksn_draw bar={.kind=KSN_RECT,.bounds={0,0,240,9},
        .clip={0,0,240,135},.opacity=255,.data.shape.color=0x304fefff};
    if(system.ops->begin(system.ctx,KSN_REPLACE,&tx)!=KSN_OK||
       system.ops->add(system.ctx,tx,&bar,&ref)!=KSN_OK||
       system.ops->end(system.ctx,tx)!=KSN_OK||
       ksn_proc_layers_present(&layers,s.core,&display,&stats)!=KSN_OK)goto fail;
    if(s.surface[1]){
        if(!prepare(&s,&grid,0)||!ksn_proc_layers_stage(&layers,1,s.frame)||
           ksn_proc_layers_present(&layers,s.core,&display,&stats)!=KSN_OK)goto fail;
    }

    int64_t prep_sum=0,stage_sum=0,present_sum=0,send_sum=0;
    int64_t prep_max=0,present_max=0;
    uint32_t bytes=0,bands=0;
    for(unsigned i=0;i<60;i++){
        float baseline=(float)(66+(int)(i%30)-15);
        int64_t started=esp_timer_get_time();
        if(!prepare(&s,&wave,baseline))goto fail;
        int64_t prep_us=esp_timer_get_time()-started;
        started=esp_timer_get_time();
        if(!ksn_proc_layers_stage(&layers,0,s.frame))goto fail;
        int64_t stage_us=esp_timer_get_time()-started;
        port.send_us=0;
        started=esp_timer_get_time();
        if(ksn_proc_layers_present(&layers,s.core,&display,&stats)!=KSN_OK)goto fail;
        int64_t present_us=esp_timer_get_time()-started;
        prep_sum+=prep_us;stage_sum+=stage_us;present_sum+=present_us;
        send_sum+=port.send_us;
        if(prep_us>prep_max)prep_max=prep_us;
        if(present_us>present_max)present_max=present_us;
        bytes+=stats.transferred_bytes;bands+=(uint32_t)ksn_render_band_count(stats.bands);
        if(i==0||i==29||i==59)
            ESP_LOGI(TAG,"FRAME i=%u segments=%u steps=%lu bands=%lx bytes=%lu prep_us=%lld stage_us=%lld compose_us=%lld lcd_us=%lld present_us=%lld",
                i,(unsigned)s.frame->count,(unsigned long)s.vm->steps,
                (unsigned long)stats.bands,(unsigned long)stats.transferred_bytes,
                (long long)prep_us,(long long)stage_us,
                (long long)(present_us-port.send_us),(long long)port.send_us,
                (long long)present_us);
        vTaskDelay(pdMS_TO_TICKS(33));
    }
    ksn_core_invalidate(s.core);
    board_capture(true);
    ksn_result captured=ksn_proc_layers_present(&layers,s.core,&display,&stats);
    board_capture(false);
    if(captured!=KSN_OK)goto fail;
    display_ok=true;
    ESP_LOGI(TAG,"DISPLAY PASS frames=60 surfaces=%u prep_mean_us=%lld prep_max_us=%lld stage_mean_us=%lld compose_mean_us=%lld lcd_mean_us=%lld present_mean_us=%lld present_max_us=%lld total_bytes=%lu total_bands=%lu free=%u largest=%u min_free=%u stack_free=%u",
        (unsigned)layers.count,(long long)(prep_sum/60),(long long)prep_max,
        (long long)(stage_sum/60),(long long)((present_sum-send_sum)/60),
        (long long)(send_sum/60),(long long)(present_sum/60),(long long)present_max,
        (unsigned long)bytes,(unsigned long)bands,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
        (unsigned)uxTaskGetStackHighWaterMark(NULL));
    goto done;
fail:
    ESP_LOGE(TAG,"FAIL stage/present free=%u largest=%u",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
done:
    release(&s);
    ESP_LOGI(TAG,"END free=%u largest=%u",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    bool compiler_ok=ksn_proc_compiler_device_probe_run();
    if (display_ok && compiler_ok) ESP_LOGI(TAG,"ALL PASS display=1 compiler_kernel=1");
    else ESP_LOGE(TAG,"ALL FAIL display=%u compiler_kernel=%u",
                  (unsigned)display_ok,(unsigned)compiler_ok);
}
#endif
