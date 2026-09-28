#include "ksn_pixel_span_device_probe.h"
#ifdef KASANE_PROC_DEVICE_PROBE
#include "ksn_pixel_function.h"
#include "ksn_pixel_span.h"
#include "ksn_render.h"
#include "board.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#define TAG "KSN_PROC"
#define WIDTH 112u
#define HEIGHT 63u
#define DIGEST_SEED UINT64_C(1469598103934665603)

typedef struct {
    ksn_pixel_pool pool;
    ksn_image_port raw;
    bool use_span,cached;
    uint16_t cached_y;
    uint8_t cached_slot;
    uint64_t cached_generation;
    uint16_t color[WIDTH];
    uint8_t alpha[WIDTH];
    int64_t eval_us;
    unsigned row_builds;
} scene_image;

typedef struct {
    int64_t send_us,verify_us;
    uint64_t digest;
    uint32_t bytes;
    unsigned sends,fail_on_send;
    uint16_t sample,first_sample;
    bool sample_seen,first_seen;
} scene_display;

typedef struct {
    ksn_core *core;
    ksn_core_command_block *commands[2];
    ksn_core_text_block *text[2];
    scene_image *image;
    ksn_client app;
    ksn_resource resource;
    ksn_ref image_ref,overlay_ref;
    ksn_pixel_frame frame;
    ksn_display_port display;
    scene_display output;
} scene_state;

typedef struct {
    uint64_t digest,fault_digest;
    uint32_t bytes,fault_bytes;
    int64_t eval_us,render_us,send_us,verify_us,frame_us;
    int64_t render_max_us,frame_max_us;
    unsigned row_builds;
} scene_metrics;

static void *internal_alloc(size_t bytes){
    return heap_caps_calloc(1,bytes,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
}

static ksn_result scene_read(void *ctx,uint16_t variant,uint16_t frame,
                             uint16_t y,uint16_t x,uint16_t count,
                             uint16_t *rgb565,uint8_t *alpha){
    scene_image *image=ctx;
    if(!image||variant||frame>=KSN_PIXEL_SLOTS||y>=HEIGHT||x>WIDTH||
       count>WIDTH-x||(count&&(!rgb565||!alpha)))return KSN_INVALID;
    ksn_result valid=image->raw.read_span(image->raw.ctx,variant,frame,
                                          y,x,0,NULL,NULL);
    if(valid!=KSN_OK||!count)return valid;
    int64_t started=esp_timer_get_time();
    ksn_pixel_slot *slot=&image->pool.slots[frame];
    bool new_row=!slot->image.cached||slot->image.cached_y!=y;
    if(image->use_span){
        if(!image->cached||image->cached_y!=y||
           image->cached_slot!=frame||
           image->cached_generation!=slot->generation){
            if(!ksn_pixel_span_eval(&slot->image,y,0,WIDTH,
                                    image->color,image->alpha))return KSN_INVALID;
            image->cached=true;image->cached_y=y;
            image->cached_slot=(uint8_t)frame;
            image->cached_generation=slot->generation;
            image->row_builds++;
        }
        memcpy(rgb565,image->color+x,(size_t)count*sizeof(uint16_t));
        memcpy(alpha,image->alpha+x,count);
    }else{
        valid=image->raw.read_span(image->raw.ctx,variant,frame,y,x,count,
                                    rgb565,alpha);
        if(valid!=KSN_OK)return valid;
        if(new_row)image->row_builds++;
    }
    image->eval_us+=esp_timer_get_time()-started;
    return KSN_OK;
}

static uint16_t *scene_strip(void *ctx){(void)ctx;return board_strip();}

static void digest_word(scene_display *out,uint16_t word){
    out->digest=(out->digest^word)*UINT64_C(1099511628211);
}

static void digest_boundary(scene_display *out,uint16_t phase){
    digest_word(out,0xfffeu);
    digest_word(out,phase);
}

static void digest_pixels(scene_display *out,uint16_t x,uint16_t y,
                           uint16_t cols,uint16_t rows,const uint16_t *pixels){
    digest_word(out,x);digest_word(out,y);
    digest_word(out,cols);digest_word(out,rows);
    for(unsigned row=0;row<rows;row++)for(unsigned col=0;col<cols;col++){
        uint16_t pixel=pixels[(size_t)row*240+x+col];
        digest_word(out,pixel);
        if(x+col==90&&y+row==35){
            out->first_sample=pixel;out->first_seen=true;
        }
        /* Steps 121/122 both cover (90,50) with the translucent overlay. */
        if(x+col==90&&y+row==50){out->sample=pixel;out->sample_seen=true;}
    }
    out->bytes+=(uint32_t)cols*rows*2u;
}

static ksn_result scene_send(void *ctx,uint16_t y,uint16_t rows,
                              const uint16_t *pixels){
    scene_display *out=ctx;
    if(pixels!=board_strip())return KSN_INVALID;
    if(++out->sends==out->fail_on_send)return KSN_IO;
    int64_t verify_started=esp_timer_get_time();
    digest_pixels(out,0,y,240,rows,pixels);
    out->verify_us+=esp_timer_get_time()-verify_started;
    int64_t started=esp_timer_get_time();
    esp_err_t result=board_present_sync(y,rows,board_strip());
    out->send_us+=esp_timer_get_time()-started;
    return result==ESP_OK?KSN_OK:KSN_IO;
}

static ksn_result scene_send_rect(void *ctx,uint16_t x,uint16_t y,
                                   uint16_t cols,uint16_t rows,
                                   const uint16_t *pixels){
    scene_display *out=ctx;
    if(pixels!=board_strip())return KSN_INVALID;
    if(++out->sends==out->fail_on_send)return KSN_IO;
    int64_t verify_started=esp_timer_get_time();
    digest_pixels(out,x,y,cols,rows,pixels);
    out->verify_us+=esp_timer_get_time()-verify_started;
    int64_t started=esp_timer_get_time();
    esp_err_t result=board_present_rect_sync(x,y,cols,rows,board_strip());
    out->send_us+=esp_timer_get_time()-started;
    return result==ESP_OK?KSN_OK:KSN_IO;
}

static void init_frame(ksn_pixel_frame *frame){
    *frame=(ksn_pixel_frame){.width=WIDTH,.height=HEIGHT,.count=8,
                             .color_reg=6,.alpha_reg=4};
    frame->code[0]=(ksn_pixel_instruction){.op=KSN_PIXEL_X,.dst=0};
    frame->code[1]=(ksn_pixel_instruction){.op=KSN_PIXEL_Y,.dst=1};
    frame->code[2]=(ksn_pixel_instruction){.op=KSN_PIXEL_PARAM,.dst=2};
    frame->code[3]=(ksn_pixel_instruction){.op=KSN_PIXEL_ADD,.dst=0,.a=0,.b=2};
    frame->code[4]=(ksn_pixel_instruction){.op=KSN_PIXEL_MUL,.dst=3,.a=0,.b=1};
    frame->code[5]=(ksn_pixel_instruction){.op=KSN_PIXEL_SHR,.dst=4,.a=3,
                                             .immediate=2};
    frame->code[6]=(ksn_pixel_instruction){.op=KSN_PIXEL_XOR,.dst=5,.a=4,.b=2};
    frame->code[7]=(ksn_pixel_instruction){.op=KSN_PIXEL_AND,.dst=6,.a=5,.b=0};
}

static bool stage_patch(scene_state *s,unsigned step,ksn_tx *tx,
                         ksn_pixel_handle *handle){
    s->frame.params[0]=(uint16_t)((step*29u+17u)&1023u);
    if(ksn_pixel_pool_stage(&s->image->pool,&s->frame,NULL,handle)!=KSN_PIXEL_OK)
        return false;
    s->image->cached=false;
    ksn_change image_change={.property=KSN_SET_IMAGE_FRAME,
        .value.image={0,handle->slot}};
    unsigned x=70u+step%60u,y=40u+(step/4u)%20u;
    ksn_change overlay_change={.property=KSN_SET_RECT,
        .value.rect={(int16_t)x,(int16_t)y,(int16_t)(x+24u),(int16_t)(y+12u)}};
    if(s->app.ops->begin(s->app.ctx,KSN_PATCH,tx)!=KSN_OK)return false;
    if(s->app.ops->change(s->app.ctx,*tx,s->image_ref,&image_change)!=KSN_OK||
       s->app.ops->change(s->app.ctx,*tx,s->overlay_ref,&overlay_change)!=KSN_OK||
       s->app.ops->end(s->app.ctx,*tx)!=KSN_OK){
        s->app.ops->abort(s->app.ctx,*tx);return false;
    }
    return true;
}

static void release_scene(scene_state *s){
    heap_caps_free(s->image);
    for(unsigned i=0;i<2;i++){
        heap_caps_free(s->commands[i]);heap_caps_free(s->text[i]);
    }
    heap_caps_free(s->core);
}

static bool run_scene(bool use_span,scene_metrics *metrics){
    const char *arm=use_span?"span":"original";
    scene_state s={0};
    const char *reason="alloc";
    unsigned at=0;
    bool ok=false;
    s.core=internal_alloc(sizeof *s.core);
    for(unsigned i=0;i<2;i++){
        s.commands[i]=internal_alloc(sizeof *s.commands[i]);
        s.text[i]=internal_alloc(sizeof *s.text[i]);
    }
    s.image=internal_alloc(sizeof *s.image);
    if(!s.core||!s.commands[0]||!s.commands[1]||!s.text[0]||!s.text[1]||
       !s.image)goto done;
    reason="init";
    if(ksn_core_bind(s.core,s.commands[0],s.commands[1],
                     s.text[0],s.text[1])!=KSN_OK||
       !ksn_pixel_pool_init(&s.image->pool,WIDTH,HEIGHT,NULL,0))goto done;
    s.image->use_span=use_span;
    ksn_pixel_pool_port(&s.image->pool,&s.image->raw);
    init_frame(&s.frame);
    ksn_pixel_handle initial={0};
    if(ksn_pixel_pool_stage(&s.image->pool,&s.frame,NULL,&initial)!=KSN_PIXEL_OK)
        goto done;
    ksn_image_port image_port={.ctx=s.image,.width=WIDTH,.height=HEIGHT,
        .variants=1,.frames=KSN_PIXEL_SLOTS,.read_span=scene_read,.opaque=false};
    if(ksn_core_register_image(s.core,KSN_APP,&image_port,&s.resource)!=KSN_OK)
        goto done;
    s.app=ksn_core_client(s.core,KSN_APP);
    s.output.digest=DIGEST_SEED;
    s.display=(ksn_display_port){.ctx=&s.output,.strip=scene_strip,
        .present=scene_send,.present_rect=scene_send_rect,
        .width=240,.height=135,.strip_rows=8};
    ksn_draw image_draw={.kind=KSN_IMAGE,.bounds={64,32,176,95},
        .clip={0,0,240,135},.opacity=255,
        .data.image={.resource=s.resource,.frame=initial.slot,
                     .scale=KSN_IMAGE_1X}};
    ksn_draw overlay={.kind=KSN_RECT,.bounds={70,40,94,52},
        .clip={0,0,240,135},.opacity=128,
        .data.shape={.color=0x80c0ffff}};
    ksn_tx tx;ksn_render_stats stats;
    if(s.app.ops->begin(s.app.ctx,KSN_REPLACE,&tx)!=KSN_OK||
       s.app.ops->background(s.app.ctx,tx,0x000000ff)!=KSN_OK||
       s.app.ops->add(s.app.ctx,tx,&image_draw,&s.image_ref)!=KSN_OK||
       s.app.ops->add(s.app.ctx,tx,&overlay,&s.overlay_ref)!=KSN_OK||
       s.app.ops->end(s.app.ctx,tx)!=KSN_OK||
       ksn_render_rects(s.core,&s.display,&stats)!=KSN_OK||
       !ksn_pixel_pool_finish(&s.image->pool,initial,true,true))goto done;
    *metrics=(scene_metrics){0};
    s.output.digest=DIGEST_SEED;s.output.bytes=0;
    reason="frame";
    for(at=0;at<120;at++){
        int64_t frame_started=esp_timer_get_time();
        ksn_pixel_handle candidate={0};
        if(!stage_patch(&s,at+1u,&tx,&candidate))goto done;
        s.image->eval_us=0;s.image->row_builds=0;
        s.output.send_us=0;s.output.verify_us=0;
        s.output.sends=0;s.output.fail_on_send=0;
        digest_boundary(&s.output,(uint16_t)(at+1u));
        int64_t render_started=esp_timer_get_time();
        if(ksn_render_rects(s.core,&s.display,&stats)!=KSN_OK)goto done;
        int64_t render_us=esp_timer_get_time()-render_started;
        if(!ksn_pixel_pool_finish(&s.image->pool,candidate,true,true))goto done;
        int64_t frame_us=esp_timer_get_time()-frame_started;
        metrics->eval_us+=s.image->eval_us;
        metrics->render_us+=render_us;
        metrics->send_us+=s.output.send_us;
        metrics->verify_us+=s.output.verify_us;
        metrics->frame_us+=frame_us;
        metrics->row_builds+=s.image->row_builds;
        if(render_us>metrics->render_max_us)metrics->render_max_us=render_us;
        if(frame_us>metrics->frame_max_us)metrics->frame_max_us=frame_us;
    }
    metrics->digest=s.output.digest;
    metrics->bytes=s.output.bytes;
    ESP_LOGI(TAG,"PIXEL_SPAN_SCENE arm=%s frames=120 eval_mean_us=%lld render_mean_us=%lld render_core_mean_us=%lld send_mean_us=%lld verify_mean_us=%lld frame_mean_us=%lld render_max_us=%lld frame_max_us=%lld row_builds=%u bytes=%u digest=%llx stack_free=%u heap_free=%u largest=%u",
        arm,(long long)(metrics->eval_us/120),(long long)(metrics->render_us/120),
        (long long)((metrics->render_us-metrics->eval_us-metrics->send_us-
                     metrics->verify_us)/120),
        (long long)(metrics->send_us/120),(long long)(metrics->verify_us/120),
        (long long)(metrics->frame_us/120),
        (long long)metrics->render_max_us,(long long)metrics->frame_max_us,
        metrics->row_builds,metrics->bytes,(unsigned long long)metrics->digest,
        (unsigned)uxTaskGetStackHighWaterMark(NULL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));

    /* Count send geometry, frame boundaries and repair traffic separately from
     * the normal 120-frame digest, then compare both backends. */
    s.output.digest=DIGEST_SEED;s.output.bytes=0;
    /* A second-band LCD failure seals the candidate. Retry exactly it. */
    reason="retry";
    ksn_pixel_handle retry={0};
    if(!stage_patch(&s,121u,&tx,&retry))goto done;
    s.output.sends=0;s.output.fail_on_send=2;s.output.first_seen=false;
    digest_boundary(&s.output,1211);
    if(ksn_render_rects(s.core,&s.display,&stats)!=KSN_IO||
       s.output.sends!=2||!s.output.first_seen||
       s.output.first_sample!=0x00a0u||
       ksn_pixel_pool_stage(&s.image->pool,&s.frame,NULL,&initial)!=KSN_PIXEL_BUSY)
        goto done;
    s.output.sends=0;s.output.fail_on_send=0;
    s.output.sample_seen=false;s.output.first_seen=false;
    digest_boundary(&s.output,1212);
    if(ksn_render_rects(s.core,&s.display,&stats)!=KSN_OK||
       !s.output.sample_seen||!s.output.first_seen||
       s.output.first_sample!=0x00a0u||
       !ksn_pixel_pool_finish(&s.image->pool,retry,true,true))goto done;
    uint16_t committed_sample=s.output.sample;
    uint32_t retry_bytes=stats.transferred_bytes;

    /* Discard a different partially sent candidate. Repair reads the old
     * committed slot and restores its composited sample. */
    reason="discard_repair";
    ksn_pixel_handle discarded={0};
    if(!stage_patch(&s,122u,&tx,&discarded))goto done;
    s.output.sends=0;s.output.fail_on_send=2;s.output.first_seen=false;
    digest_boundary(&s.output,1221);
    if(ksn_render_rects(s.core,&s.display,&stats)!=KSN_IO||
       s.output.sends!=2||!s.output.first_seen||
       s.output.first_sample!=0x009cu||
       ksn_core_discard(s.core,tx)!=KSN_OK||
       !ksn_pixel_pool_finish(&s.image->pool,discarded,false,false)||
       !s.image->pool.repair_required)goto done;
    s.output.sends=0;s.output.fail_on_send=0;
    s.output.sample_seen=false;s.output.first_seen=false;
    digest_boundary(&s.output,1222);
    if(ksn_render_rects(s.core,&s.display,&stats)!=KSN_OK||
       !s.output.sample_seen||s.output.sample!=committed_sample||
       !s.output.first_seen||s.output.first_sample!=0x00a0u)goto done;
    ksn_pixel_pool_repair_done(&s.image->pool);
    if(s.image->pool.repair_required)goto done;
    uint32_t repair_bytes=stats.transferred_bytes;
    unsigned repair_sends=s.output.sends;
    reason="post_repair_present";
    ksn_pixel_handle after_repair={0};
    if(!stage_patch(&s,123u,&tx,&after_repair))goto done;
    s.output.sends=0;s.output.first_seen=false;
    digest_boundary(&s.output,1231);
    if(ksn_render_rects(s.core,&s.display,&stats)!=KSN_OK||
       !s.output.first_seen||
       !ksn_pixel_pool_finish(&s.image->pool,after_repair,true,true))goto done;
    metrics->fault_digest=s.output.digest;
    metrics->fault_bytes=s.output.bytes;
    ESP_LOGI(TAG,"PIXEL_SPAN_REPAIR PASS arm=%s retry_bytes=%u repair_bytes=%u first_discard=009c first_repair=00a0 overlay=%04x repair_sends=%u post_repair_bytes=%u fault_bytes=%u fault_digest=%llx stack_free=%u",
        arm,retry_bytes,repair_bytes,committed_sample,
        repair_sends,stats.transferred_bytes,metrics->fault_bytes,
        (unsigned long long)metrics->fault_digest,
        (unsigned)uxTaskGetStackHighWaterMark(NULL));
    ok=true;
done:
    if(!ok)ESP_LOGE(TAG,"PIXEL_SPAN_SCENE FAIL arm=%s reason=%s frame=%u free=%u largest=%u",
        arm,reason,at,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    release_scene(&s);
    return ok;
}

bool ksn_pixel_span_device_probe_run(void){
    scene_metrics original={0},span={0};
    bool original_ok=run_scene(false,&original);
    bool span_ok=run_scene(true,&span);
    bool same=original_ok&&span_ok&&original.digest==span.digest&&
              original.bytes==span.bytes&&
              original.fault_digest==span.fault_digest&&
              original.fault_bytes==span.fault_bytes;
    if(same)ESP_LOGI(TAG,"PIXEL_SPAN_COMPARE PASS frames=120 bytes=%u digest=%llx fault_bytes=%u fault_digest=%llx original_frame_mean_us=%lld span_frame_mean_us=%lld",
        original.bytes,(unsigned long long)original.digest,
        original.fault_bytes,(unsigned long long)original.fault_digest,
        (long long)(original.frame_us/120),(long long)(span.frame_us/120));
    else ESP_LOGE(TAG,"PIXEL_SPAN_COMPARE FAIL original=%u span=%u original_bytes=%u span_bytes=%u original_digest=%llx span_digest=%llx original_fault_bytes=%u span_fault_bytes=%u original_fault_digest=%llx span_fault_digest=%llx",
        (unsigned)original_ok,(unsigned)span_ok,original.bytes,span.bytes,
        (unsigned long long)original.digest,(unsigned long long)span.digest,
        original.fault_bytes,span.fault_bytes,
        (unsigned long long)original.fault_digest,
        (unsigned long long)span.fault_digest);
    return same;
}
#endif
