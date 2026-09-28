#include "ksn_procedural_present.h"
#ifdef KASANE_PROC_DEVICE_PROBE
#include "ksn_proc_compiler_device_probe.h"
#include "ksn_video_frames.h"
#include "ksn_depth_lines.h"
#include "ksn_pixel_function.h"
#include "ksn_pixel_span.h"
#include "ksn_pixel_span_device_probe.h"
#include "board.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
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
typedef struct {
    int64_t send_us;
    uint16_t sample;
    unsigned sends,fail_on_send;
    uint16_t sample_x,sample_y;
    uint16_t sample2,sample2_x,sample2_y;
    bool sample_seen,sample2_seen,sample2_enabled;
} probe_port;
static uint16_t *strip(void *ctx){(void)ctx;return board_strip();}
static ksn_result present(void *ctx,uint16_t y,uint16_t rows,const uint16_t *pixels){
    probe_port *port=(probe_port *)ctx;
    if(pixels!=board_strip())return KSN_INVALID;
    if(++port->sends==port->fail_on_send)return KSN_IO;
    if(y<=port->sample_y&&port->sample_y<y+rows){
        port->sample=pixels[(port->sample_y-y)*KSN_PROC_W+port->sample_x];
        port->sample_seen=true;
    }
    if(port->sample2_enabled&&y<=port->sample2_y&&port->sample2_y<y+rows){
        port->sample2=pixels[(port->sample2_y-y)*KSN_PROC_W+port->sample2_x];
        port->sample2_seen=true;
    }
    int64_t started=esp_timer_get_time();
    esp_err_t result=board_present_sync(y,rows,board_strip());
    port->send_us+=esp_timer_get_time()-started;
    return result==ESP_OK?KSN_OK:KSN_IO;
}
static ksn_result present_rect(void *ctx,uint16_t x,uint16_t y,uint16_t cols,
                               uint16_t rows,const uint16_t *pixels){
    probe_port *port=(probe_port *)ctx;
    if(pixels!=board_strip())return KSN_INVALID;
    if(++port->sends==port->fail_on_send)return KSN_IO;
    if(x<=port->sample_x&&port->sample_x<x+cols&&
       y<=port->sample_y&&port->sample_y<y+rows){
        port->sample=pixels[(port->sample_y-y)*KSN_PROC_W+port->sample_x];
        port->sample_seen=true;
    }
    if(port->sample2_enabled&&x<=port->sample2_x&&port->sample2_x<x+cols&&
       y<=port->sample2_y&&port->sample2_y<y+rows){
        port->sample2=pixels[(port->sample2_y-y)*KSN_PROC_W+port->sample2_x];
        port->sample2_seen=true;
    }
    int64_t started=esp_timer_get_time();
    esp_err_t result=board_present_rect_sync(x,y,cols,rows,board_strip());
    port->send_us+=esp_timer_get_time()-started;
    return result==ESP_OK?KSN_OK:KSN_IO;
}
static bool video_produce(ksn_video_frames *frames,uint16_t color,uint64_t pts,
                          ksn_video_handle *handle){
    uint16_t *pixels;
    if(ksn_video_begin(frames,handle,&pixels)!=KSN_VIDEO_OK)return false;
    for(unsigned i=0;i<KSN_VIDEO_MAX_PIXELS;i++)pixels[i]=color;
    return ksn_video_publish(frames,*handle,pts)==KSN_VIDEO_OK&&
           ksn_video_select(frames,pts,handle)==KSN_VIDEO_OK;
}
static bool video_probe(void){
    ksn_core *core=internal_alloc(sizeof *core);
    ksn_core_command_block *commands[2]={internal_alloc(sizeof *commands[0]),
                                         internal_alloc(sizeof *commands[0])};
    ksn_core_text_block *text[2]={internal_alloc(sizeof *text[0]),
                                  internal_alloc(sizeof *text[0])};
    uint16_t *buffers[KSN_VIDEO_SLOTS]={0};
    ksn_depth_frame *mixed_depth=NULL;
    ksn_depth_image *mixed_image=NULL;
    uint8_t *mixed_reservation=NULL;
    unsigned mixed_stage=0;
    probe_port port={.sample_x=88,.sample_y=35};
    for(unsigned i=0;i<KSN_VIDEO_SLOTS;i++)
        buffers[i]=internal_alloc(KSN_VIDEO_MAX_PIXELS*sizeof(uint16_t));
    bool ok=false;
    if(!core||!commands[0]||!commands[1]||!text[0]||!text[1]||
       !buffers[0]||!buffers[1]||!buffers[2])goto done;
    ksn_video_frames frames;
    if(!ksn_video_frames_init(&frames,64,64,buffers,KSN_VIDEO_MAX_PIXELS)||
       ksn_core_bind(core,commands[0],commands[1],text[0],text[1])!=KSN_OK)
        goto done;
    ksn_image_port image_port;ksn_video_image_port(&frames,&image_port);
    ksn_resource image;
    if(ksn_core_register_image(core,KSN_APP,&image_port,&image)!=KSN_OK)goto done;
    ksn_display_port display={.ctx=&port,.strip=strip,.present=present,
        .width=240,.height=135,.strip_rows=8,.present_rect=present_rect};
    ksn_render_stats stats;
    ksn_client app=ksn_core_client(core,KSN_APP);
    ksn_tx tx;ksn_ref ref;ksn_video_handle frame;
    if(!video_produce(&frames,0x1234,100,&frame))goto done;
    ksn_draw draw={.kind=KSN_IMAGE,.bounds={88,35,152,99},
        .clip={0,0,240,135},.opacity=255,
        .data.image={.resource=image,.frame=frame.slot,.scale=KSN_IMAGE_1X}};
    if(app.ops->begin(app.ctx,KSN_REPLACE,&tx)!=KSN_OK||
       app.ops->background(app.ctx,tx,0x000000ff)!=KSN_OK||
       app.ops->add(app.ctx,tx,&draw,&ref)!=KSN_OK||
       app.ops->end(app.ctx,tx)!=KSN_OK||
       ksn_render_rects(core,&display,&stats)!=KSN_OK||
       !ksn_video_finish(&frames,frame,true)||port.sample!=0x1234)goto done;
    if(!video_produce(&frames,0x5678,200,&frame))goto done;
    ksn_change change={.property=KSN_SET_IMAGE_FRAME,
                       .value.image={0,frame.slot}};
    if(app.ops->begin(app.ctx,KSN_PATCH,&tx)!=KSN_OK||
       app.ops->change(app.ctx,tx,ref,&change)!=KSN_OK||
       app.ops->end(app.ctx,tx)!=KSN_OK)goto done;
    port.sends=0;port.fail_on_send=2;
    if(ksn_render_rects(core,&display,&stats)!=KSN_IO)goto done;
    port.sends=0;port.fail_on_send=0;
    if(ksn_render_rects(core,&display,&stats)!=KSN_OK||
       !ksn_video_finish(&frames,frame,true)||port.sample!=0x5678)goto done;
    if(!video_produce(&frames,0xabcd,300,&frame))goto done;
    change.value.image.frame=frame.slot;
    if(app.ops->begin(app.ctx,KSN_PATCH,&tx)!=KSN_OK||
       app.ops->change(app.ctx,tx,ref,&change)!=KSN_OK||
       app.ops->end(app.ctx,tx)!=KSN_OK)goto done;
    port.sends=0;port.fail_on_send=2;
    if(ksn_render_rects(core,&display,&stats)!=KSN_IO||
       ksn_core_discard(core,tx)!=KSN_OK||
       !ksn_video_finish(&frames,frame,false)||
       !ksn_video_needs_repair(&frames))goto done;
    port.sends=0;port.fail_on_send=0;
    if(ksn_render_rects(core,&display,&stats)!=KSN_OK||
       port.sample!=0x5678)goto done;
    ksn_video_repair_done(&frames);
    if(ksn_video_needs_repair(&frames))goto done;
    uint64_t render_sum=0,render_max=0,bytes=0;
    for(unsigned i=0;i<30;i++){
        uint16_t color=(uint16_t)(0x1100u+i);
        if(!video_produce(&frames,color,400u+(uint64_t)i*33333u,&frame))
            goto done;
        change.value.image.frame=frame.slot;
        if(app.ops->begin(app.ctx,KSN_PATCH,&tx)!=KSN_OK||
           app.ops->change(app.ctx,tx,ref,&change)!=KSN_OK||
           app.ops->end(app.ctx,tx)!=KSN_OK)goto done;
        int64_t start=esp_timer_get_time();
        if(ksn_render_rects(core,&display,&stats)!=KSN_OK||
           !ksn_video_finish(&frames,frame,true)||port.sample!=color)
            goto done;
        uint64_t elapsed=(uint64_t)(esp_timer_get_time()-start);
        render_sum+=elapsed;
        if(elapsed>render_max)render_max=elapsed;
        bytes+=stats.transferred_bytes;
    }
    ESP_LOGI(TAG,"VIDEO_STREAM frames=30 mean_us=%llu max_us=%llu bytes=%llu",
        (unsigned long long)(render_sum/30u),
        (unsigned long long)render_max,(unsigned long long)bytes);
    /* Combine three owners of pixels under a bounded heap reservation. The
     * video frame remains pinned until the failed LCD transfer is retried. */
    mixed_depth=internal_alloc(sizeof *mixed_depth);
    mixed_image=internal_alloc(sizeof *mixed_image);
    mixed_reservation=internal_alloc(16384);
    if(!mixed_depth||!mixed_image||!mixed_reservation)goto done;
    mixed_stage=1;
    ksn_depth_frame_reset(mixed_depth);
    if(!ksn_depth_add(mixed_depth,(ksn_depth_line){0,8,15,8,100,100,0xf800})||
       !ksn_depth_image_init(mixed_image,mixed_depth,16,16))goto done;
    mixed_stage=2;
    ksn_image_port mixed_port;ksn_depth_image_port(mixed_image,&mixed_port);
    ksn_resource depth_resource;
    if(ksn_core_register_image(core,KSN_APP,&mixed_port,&depth_resource)!=KSN_OK)
        goto done;
    mixed_stage=3;
    ksn_draw depth_draw={.kind=KSN_IMAGE,.bounds={88,35,104,51},
        .clip={0,0,240,135},.opacity=255,
        .data.image={.resource=depth_resource,.scale=KSN_IMAGE_1X}};
    draw.data.image.frame=frame.slot;
    ksn_ref depth_ref;
    if(app.ops->begin(app.ctx,KSN_REPLACE,&tx)!=KSN_OK||
       app.ops->background(app.ctx,tx,0x000000ff)!=KSN_OK||
       app.ops->add(app.ctx,tx,&draw,&ref)!=KSN_OK||
       app.ops->add(app.ctx,tx,&depth_draw,&depth_ref)!=KSN_OK||
       app.ops->end(app.ctx,tx)!=KSN_OK||
       ksn_render_rects(core,&display,&stats)!=KSN_OK)goto done;
    mixed_stage=4;
    ksn_client system=ksn_core_client(core,KSN_SYSTEM);
    ksn_draw notice={.kind=KSN_RECT,.bounds={90,40,98,48},
        .clip={0,0,240,135},.opacity=128,
        .data.shape={0x00ff00ff,0,0}};
    ksn_ref notice_ref;
    if(system.ops->begin(system.ctx,KSN_REPLACE,&tx)!=KSN_OK||
       system.ops->add(system.ctx,tx,&notice,&notice_ref)!=KSN_OK||
       system.ops->end(system.ctx,tx)!=KSN_OK||
       ksn_render_rects(core,&display,&stats)!=KSN_OK)goto done;
    mixed_stage=5;
    port.sample_x=89;port.sample_y=43;
    port.sample_seen=false;
    if(!ksn_core_invalidate_image(core,depth_resource)||
       ksn_render_rects(core,&display,&stats)!=KSN_OK||
       !port.sample_seen||port.sample!=0xf800)
        goto done;
    mixed_stage=6;
    port.sample_x=92;port.sample_y=41;
    port.sample_seen=false;
    if(!ksn_core_invalidate_image(core,image)||
       ksn_render_rects(core,&display,&stats)!=KSN_OK||
       !port.sample_seen||port.sample!=0x0c8e)goto done;
    mixed_stage=7;
    if(!video_produce(&frames,0x7777,400u+30u*33333u,&frame))goto done;
    mixed_stage=8;
    change.value.image.frame=frame.slot;
    if(app.ops->begin(app.ctx,KSN_PATCH,&tx)!=KSN_OK||
       app.ops->change(app.ctx,tx,ref,&change)!=KSN_OK||
       app.ops->end(app.ctx,tx)!=KSN_OK)goto done;
    port.sends=0;port.fail_on_send=2;
    if(ksn_render_rects(core,&display,&stats)!=KSN_IO)goto done;
    mixed_stage=9;
    port.sends=0;port.fail_on_send=0;
    if(ksn_render_rects(core,&display,&stats)!=KSN_OK||
       !ksn_video_finish(&frames,frame,true))goto done;
    mixed_stage=10;
    notice.bounds=(ksn_rect){110,40,118,48};
    port.sample_seen=false;
    if(system.ops->begin(system.ctx,KSN_REPLACE,&tx)!=KSN_OK||
       system.ops->add(system.ctx,tx,&notice,&notice_ref)!=KSN_OK||
       system.ops->end(system.ctx,tx)!=KSN_OK||
       ksn_render_rects(core,&display,&stats)!=KSN_OK||
       !port.sample_seen||port.sample!=0x7777)
        goto done;
    mixed_stage=11;
    port.sample_x=112;port.sample_y=41;
    port.sample_seen=false;
    port.sample2_enabled=true;port.sample2_x=89;port.sample2_y=36;
    port.sample2_seen=false;
    if(ksn_core_reset_layer(core,KSN_APP)!=KSN_OK||
       ksn_render_rects(core,&display,&stats)!=KSN_OK||
       !port.sample_seen||port.sample!=0x0400||
       !port.sample2_seen||port.sample2!=0)
        goto done;
    mixed_stage=12;
    ESP_LOGI(TAG,"COMBINED PASS video=1 depth=1 notice=1 lowheap=16384 retry=1 teardown=1 free=%u largest=%u",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    ok=true;
done:
    ESP_LOGI(TAG,"VIDEO %s stage=%u sample=%04x sends=%u slots=%u pixels=%u free=%u largest=%u min_free=%u stack_free=%u",
        ok?"PASS":"FAIL",mixed_stage,(unsigned)port.sample,port.sends,
        (unsigned)KSN_VIDEO_SLOTS,(unsigned)KSN_VIDEO_MAX_PIXELS,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
        (unsigned)uxTaskGetStackHighWaterMark(NULL));
    for(unsigned i=0;i<KSN_VIDEO_SLOTS;i++)heap_caps_free(buffers[i]);
    heap_caps_free(mixed_reservation);
    heap_caps_free(mixed_image);
    heap_caps_free(mixed_depth);
    for(unsigned i=0;i<2;i++){heap_caps_free(commands[i]);heap_caps_free(text[i]);}
    heap_caps_free(core);
    return ok;
}
static bool depth_probe(void){
    ksn_core *core=internal_alloc(sizeof *core);
    ksn_core_command_block *commands[2]={internal_alloc(sizeof *commands[0]),
                                         internal_alloc(sizeof *commands[0])};
    ksn_core_text_block *text[2]={internal_alloc(sizeof *text[0]),
                                  internal_alloc(sizeof *text[0])};
    ksn_depth_frame *frames[2]={internal_alloc(sizeof(ksn_depth_frame)),
                                internal_alloc(sizeof(ksn_depth_frame))};
    ksn_depth_image *image=internal_alloc(sizeof *image);
    bool ok=false;
    if(!core||!commands[0]||!commands[1]||!text[0]||!text[1]||
       !frames[0]||!frames[1]||!image)goto done;
    ksn_depth_frame_reset(frames[0]);ksn_depth_frame_reset(frames[1]);
    if(!ksn_depth_add(frames[0],(ksn_depth_line){0,32,63,32,300,300,0xf800})||
       !ksn_depth_add(frames[0],(ksn_depth_line){32,0,32,63,100,100,0x07e0})||
       !ksn_depth_add(frames[1],(ksn_depth_line){0,32,63,32,50,50,0x001f})||
       !ksn_depth_image_init(image,frames[0],64,64)||
       ksn_core_bind(core,commands[0],commands[1],text[0],text[1])!=KSN_OK)
        goto done;
    ksn_image_port image_port;ksn_depth_image_port(image,&image_port);
    ksn_resource resource;
    if(ksn_core_register_image(core,KSN_APP,&image_port,&resource)!=KSN_OK)goto done;
    probe_port port={.sample_x=120,.sample_y=67};
    ksn_display_port display={.ctx=&port,.strip=strip,.present=present,
        .width=240,.height=135,.strip_rows=8,.present_rect=present_rect};
    ksn_render_stats stats;
    ksn_client app=ksn_core_client(core,KSN_APP);
    ksn_tx tx;ksn_ref ref;
    ksn_draw draw={.kind=KSN_IMAGE,.bounds={88,35,152,99},
        .clip={0,0,240,135},.opacity=255,
        .data.image={.resource=resource,.scale=KSN_IMAGE_1X}};
    if(app.ops->begin(app.ctx,KSN_REPLACE,&tx)!=KSN_OK||
       app.ops->background(app.ctx,tx,0x203040ff)!=KSN_OK||
       app.ops->add(app.ctx,tx,&draw,&ref)!=KSN_OK||
       app.ops->end(app.ctx,tx)!=KSN_OK||
       ksn_render_rects(core,&display,&stats)!=KSN_OK||
       port.sample!=0x07e0)goto done;
    ksn_depth_image_bind(image,frames[1]);
    if(!ksn_core_invalidate_image(core,resource))goto done;
    port.sends=0;
    int64_t started=esp_timer_get_time();
    if(ksn_render_rects(core,&display,&stats)!=KSN_OK||
       port.sample!=0x001f)goto done;
    int64_t redraw_us=esp_timer_get_time()-started;
    uint32_t redraw_bytes=stats.transferred_bytes;
    ksn_depth_image_bind(image,frames[0]);
    if(!ksn_core_invalidate_image(core,resource))goto done;
    port.sends=0;port.fail_on_send=2;
    if(ksn_render_rects(core,&display,&stats)!=KSN_IO)goto done;
    port.sends=0;port.fail_on_send=0;
    if(ksn_render_rects(core,&display,&stats)!=KSN_OK||
       port.sample!=0x07e0)goto done;
    ESP_LOGI(TAG,"DEPTH_IMAGE PASS redraw_us=%lld redraw_bytes=%u repair_bytes=%u free=%u largest=%u",
        (long long)redraw_us,(unsigned)redraw_bytes,
        (unsigned)stats.transferred_bytes,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    ok=true;
done:
    if(!ok)ESP_LOGE(TAG,"DEPTH_IMAGE FAIL");
    heap_caps_free(image);
    for(unsigned i=0;i<2;i++){
        heap_caps_free(frames[i]);heap_caps_free(commands[i]);heap_caps_free(text[i]);
    }
    heap_caps_free(core);
    return ok;
}
static bool pixel_probe(void){
    ksn_core *core=internal_alloc(sizeof *core);
    ksn_core_command_block *commands[2]={internal_alloc(sizeof *commands[0]),
                                         internal_alloc(sizeof *commands[0])};
    ksn_core_text_block *text[2]={internal_alloc(sizeof *text[0]),
                                  internal_alloc(sizeof *text[0])};
    ksn_pixel_image *image=internal_alloc(sizeof *image);
    bool ok=false;
    if(!core||!commands[0]||!commands[1]||!text[0]||!text[1]||!image)
        goto done;
    ksn_pixel_frame frame={.width=64,.height=64,.count=2,
        .color_reg=0,.alpha_reg=1};
    frame.code[0]=(ksn_pixel_instruction){.op=KSN_PIXEL_PARAM,
                                          .dst=0,.immediate=0};
    frame.code[1]=(ksn_pixel_instruction){.op=KSN_PIXEL_IMM,
                                          .dst=1,.immediate=255};
    frame.params[0]=0x1234;
    if(!ksn_pixel_image_bind(image,&frame,NULL)||
       ksn_core_bind(core,commands[0],commands[1],text[0],text[1])!=KSN_OK)
        goto done;
    ksn_image_port image_port;ksn_pixel_image_port(image,&image_port);
    ksn_resource resource;
    if(ksn_core_register_image(core,KSN_APP,&image_port,&resource)!=KSN_OK)
        goto done;
    probe_port port={.sample_x=88,.sample_y=35};
    ksn_display_port display={.ctx=&port,.strip=strip,.present=present,
        .width=240,.height=135,.strip_rows=8,.present_rect=present_rect};
    ksn_render_stats stats;
    ksn_client app=ksn_core_client(core,KSN_APP);
    ksn_tx tx;ksn_ref ref;
    ksn_draw draw={.kind=KSN_IMAGE,.bounds={88,35,152,99},
        .clip={0,0,240,135},.opacity=255,
        .data.image={.resource=resource,.scale=KSN_IMAGE_1X}};
    if(app.ops->begin(app.ctx,KSN_REPLACE,&tx)!=KSN_OK||
       app.ops->background(app.ctx,tx,0x000000ff)!=KSN_OK||
       app.ops->add(app.ctx,tx,&draw,&ref)!=KSN_OK||
       app.ops->end(app.ctx,tx)!=KSN_OK||
       ksn_render_rects(core,&display,&stats)!=KSN_OK||port.sample!=0x1234)
        goto done;
    frame.params[0]=0x5678;
    if(!ksn_pixel_image_bind(image,&frame,NULL)||
       !ksn_core_invalidate_image(core,resource))goto done;
    port.sends=0;port.fail_on_send=2;
    if(ksn_render_rects(core,&display,&stats)!=KSN_IO)goto done;
    port.sends=0;port.fail_on_send=0;
    int64_t started=esp_timer_get_time();
    if(ksn_render_rects(core,&display,&stats)!=KSN_OK||port.sample!=0x5678)
        goto done;
    ESP_LOGI(TAG,"PIXEL_IMAGE PASS retry_us=%lld bytes=%u free=%u largest=%u",
        (long long)(esp_timer_get_time()-started),
        (unsigned)stats.transferred_bytes,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    ok=true;
done:
    if(!ok)ESP_LOGE(TAG,"PIXEL_IMAGE FAIL");
    heap_caps_free(image);
    for(unsigned i=0;i<2;i++){
        heap_caps_free(commands[i]);heap_caps_free(text[i]);
    }
    heap_caps_free(core);
    return ok;
}
static bool pixel_scope_probe(ksn_pixel_pool *pool,uint16_t width,
                              uint16_t height,uint8_t commands,
                              unsigned runs,const char *marker,
                              bool span_backend){
    if(!ksn_pixel_pool_init(pool,width,height,NULL,0)){
        ESP_LOGE(TAG,"%s FAIL reason=init",marker);return false;
    }
    ksn_pixel_frame frame={.width=width,.height=height,.count=commands,
                           .color_reg=0,.alpha_reg=1};
    frame.code[0]=(ksn_pixel_instruction){.op=KSN_PIXEL_X,.dst=0};
    frame.code[1]=(ksn_pixel_instruction){.op=KSN_PIXEL_IMM,.dst=1,
                                          .immediate=255};
    for(unsigned i=2;i<commands;i++)
        frame.code[i]=(ksn_pixel_instruction){.op=KSN_PIXEL_ADD,
                                               .dst=0,.a=0,.b=0};
    ksn_pixel_handle handle={0};
    if(ksn_pixel_pool_stage(pool,&frame,NULL,&handle)!=KSN_PIXEL_OK){
        ESP_LOGE(TAG,"%s FAIL reason=stage",marker);return false;
    }
    ksn_image_port port;ksn_pixel_pool_port(pool,&port);
    uint16_t color[240];uint8_t alpha[240];
    uint16_t expected=(uint16_t)((uint32_t)(width-1u)<<(commands-2u));
    int64_t sum_us=0,max_us=0,min_us=INT64_MAX;
    uint64_t checksum=0;
    for(unsigned run=0;run<runs;run++){
        int64_t started=esp_timer_get_time();
        for(unsigned y=0;y<height;y++){
            bool read_ok=span_backend?
                ksn_pixel_span_eval(&pool->slots[handle.slot].image,
                                    (uint16_t)y,0,width,color,alpha):
                port.read_span(port.ctx,0,handle.slot,(uint16_t)y,0,width,
                               color,alpha)==KSN_OK;
            if(!read_ok||color[0]!=0||
               color[width-1]!=expected||alpha[width-1]!=255){
                ESP_LOGE(TAG,"%s FAIL reason=read_or_pixels run=%u y=%u",
                    marker,run,y);return false;
            }
            checksum+=(uint32_t)color[width-1]+alpha[width-1];
        }
        int64_t us=esp_timer_get_time()-started;
        sum_us+=us;
        if(us>max_us)max_us=us;
        if(us<min_us)min_us=us;
    }
    if(checksum!=(uint64_t)runs*height*(expected+255u)||
       !ksn_pixel_pool_finish(pool,handle,true,true)){
        ESP_LOGE(TAG,"%s FAIL reason=checksum_or_finish",marker);return false;
    }
    bool budget=sum_us/runs<=33333;
    ESP_LOGI(TAG,"%s %s width=%u height=%u commands=%u visits=%u runs=%u min_us=%lld mean_us=%lld max_us=%lld checksum=%llu budget_30fps=%u stack_free=%u",
        marker,budget?"PASS":"FAIL",(unsigned)width,(unsigned)height,
        (unsigned)commands,(unsigned)((uint32_t)width*height*commands),runs,
        (long long)min_us,(long long)(sum_us/runs),(long long)max_us,
        (unsigned long long)checksum,(unsigned)budget,
        (unsigned)uxTaskGetStackHighWaterMark(NULL));
    return budget;
}
static bool pixel_memory_probe(ksn_pixel_pool *pool,uint32_t caps,
                                bool external,const char *marker){
    enum { PIXELS=240*135 };
    size_t free_before=heap_caps_get_free_size(caps);
    size_t largest_before=heap_caps_get_largest_free_block(caps);
    uint16_t *buffers[KSN_PIXEL_SLOTS]={0};
    const char *reason="alloc";
    int64_t started=esp_timer_get_time();
    for(unsigned i=0;i<KSN_PIXEL_SLOTS;i++)
        buffers[i]=heap_caps_calloc(1,PIXELS*sizeof(uint16_t),caps);
    int64_t alloc_us=esp_timer_get_time()-started;
    size_t free_alloc=heap_caps_get_free_size(caps);
    size_t largest_alloc=heap_caps_get_largest_free_block(caps);
    bool ok=buffers[0]&&buffers[1];
    if(ok){
        reason="placement";
        for(unsigned i=0;i<KSN_PIXEL_SLOTS;i++)
            if(external?!esp_ptr_external_ram(buffers[i]):
                        !esp_ptr_internal(buffers[i]))ok=false;
    }
    if(ok){
        reason="init";
        ok=ksn_pixel_pool_init(pool,240,135,buffers,PIXELS);
    }
    int64_t read_us=0;
    uint32_t read_sum=0;
    if(ok){
        /* One sequential read sweep measures placement cost; this is not a
         * pixel-function timing or an automatic PSRAM adoption decision. */
        volatile uint16_t *a=buffers[0],*b=buffers[1];
        started=esp_timer_get_time();
        for(unsigned i=0;i<PIXELS;i++)read_sum+=a[i]+b[i];
        read_us=esp_timer_get_time()-started;
        ESP_LOGI(TAG,"%s PASS pool_internal=1 buffers=%u free_before=%u largest_before=%u free_alloc=%u largest_alloc=%u alloc_us=%lld read_us=%lld read_sum=%u",
            marker,(unsigned)(KSN_PIXEL_SLOTS*PIXELS*sizeof(uint16_t)),
            (unsigned)free_before,(unsigned)largest_before,
            (unsigned)free_alloc,(unsigned)largest_alloc,
            (long long)alloc_us,(long long)read_us,(unsigned)read_sum);
    }else ESP_LOGE(TAG,"%s FAIL reason=%s pool_internal=1 buffers=%u free_before=%u largest_before=%u free_alloc=%u largest_alloc=%u alloc_us=%lld",
        marker,reason,(unsigned)(KSN_PIXEL_SLOTS*PIXELS*sizeof(uint16_t)),
        (unsigned)free_before,(unsigned)largest_before,
        (unsigned)free_alloc,(unsigned)largest_alloc,(long long)alloc_us);
    /* No resource is registered. Clear the borrowed storage pointers before
     * freeing this placement experiment's buffers. */
    if(ok)ksn_pixel_pool_init(pool,240,135,NULL,0);
    for(unsigned i=0;i<KSN_PIXEL_SLOTS;i++)heap_caps_free(buffers[i]);
    return ok;
}
static bool pixel_work_probe(void){
    ksn_pixel_pool *pool=internal_alloc(sizeof *pool);
    if(!pool||!esp_ptr_internal(pool)){
        ESP_LOGE(TAG,"PIXEL_WORK FAIL reason=pool_alloc bytes=%u free=%u largest=%u",
            (unsigned)sizeof *pool,
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
        heap_caps_free(pool);return false;
    }
    bool full_ok=pixel_scope_probe(pool,240,135,18,8,"PIXEL_WORK_TIME",false);
    bool span_ok=pixel_scope_probe(pool,240,135,18,8,"PIXEL_WORK_SPAN",true);
    bool small_a_ok=pixel_scope_probe(pool,64,64,6,16,"PIXEL_SCOPE_64",false);
    bool small_b_ok=pixel_scope_probe(pool,112,63,8,16,"PIXEL_SCOPE_112",false);
    bool internal_ok=pixel_memory_probe(pool,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT,
                                         false,"PIXEL_WORK_MEM");
    bool psram_ok=pixel_memory_probe(pool,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT,
                                      true,"PIXEL_WORK_PSRAM");
    bool ok=full_ok&&span_ok&&small_a_ok&&small_b_ok&&internal_ok;
    if(ok)ESP_LOGI(TAG,"PIXEL_WORK PASS time=1 span=1 scopes=1 memory=1 psram=%u pool=%u",
                   (unsigned)psram_ok,(unsigned)sizeof *pool);
    else ESP_LOGE(TAG,"PIXEL_WORK FAIL time=%u span=%u scopes=%u memory=%u psram=%u pool=%u",
                  (unsigned)full_ok,(unsigned)span_ok,(unsigned)(small_a_ok&&small_b_ok),
                  (unsigned)internal_ok,(unsigned)psram_ok,(unsigned)sizeof *pool);
    heap_caps_free(pool);
    return ok;
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
    if(!prepare(&s,&wave,104.0f)||
       !ksn_proc_layers_stage(&layers,0,s.frame))goto fail;
    port.sends=0;port.fail_on_send=2;
    if(ksn_proc_layers_present(&layers,s.core,&display,&stats)!=KSN_IO||
       !ksn_proc_layers_needs_repair(&layers))goto fail;
    unsigned partial_sends=port.sends;
    port.sends=0;port.fail_on_send=0;
    if(ksn_proc_layers_present(&layers,s.core,&display,&stats)!=KSN_OK||
       ksn_proc_layers_needs_repair(&layers))goto fail;
    ESP_LOGI(TAG,"REPAIR PASS partial_sends=%u repair_bands=%u repair_bytes=%lu",
        partial_sends,ksn_render_band_count(stats.bands),
        (unsigned long)stats.transferred_bytes);
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
    bool video_ok=video_probe();
    bool depth_ok=depth_probe();
    bool pixel_ok=pixel_probe();
    bool pixel_work_ok=pixel_work_probe();
    bool pixel_scene_ok=ksn_pixel_span_device_probe_run();
    if (display_ok && compiler_ok && video_ok && depth_ok && pixel_ok &&
        pixel_work_ok && pixel_scene_ok)
        ESP_LOGI(TAG,"ALL PASS display=1 compiler_kernel=1 video=1 depth=1 pixel=1 pixel_work=1 pixel_scene=1");
    else ESP_LOGE(TAG,"ALL FAIL display=%u compiler_kernel=%u video=%u depth=%u pixel=%u pixel_work=%u pixel_scene=%u",
                  (unsigned)display_ok,(unsigned)compiler_ok,
                  (unsigned)video_ok,(unsigned)depth_ok,(unsigned)pixel_ok,
                  (unsigned)pixel_work_ok,(unsigned)pixel_scene_ok);
}
#endif
