#include "ksn_pixel_function.h"
#include <string.h>

static bool valid_frame(const ksn_pixel_frame *frame,
                        const ksn_pixel_underlay *underlay){
    if(!frame||!frame->width||frame->width>240||!frame->height||
       frame->height>135||!frame->count||frame->count>KSN_PIXEL_CODE_MAX||
       (uint32_t)frame->width*frame->height*frame->count>KSN_PIXEL_WORK_MAX||
       frame->color_reg>=KSN_PIXEL_REGS||frame->alpha_reg>=KSN_PIXEL_REGS)
        return false;
    unsigned written=0;
    bool needs_underlay=false;
    for(unsigned i=0;i<frame->count;i++){
        const ksn_pixel_instruction *ins=&frame->code[i];
        if(ins->dst>=KSN_PIXEL_REGS)return false;
        switch(ins->op){
        case KSN_PIXEL_IMM: case KSN_PIXEL_X: case KSN_PIXEL_Y: break;
        case KSN_PIXEL_PARAM:
            if(ins->immediate>=KSN_PIXEL_PARAMS)return false;
            break;
        case KSN_PIXEL_UNDERLAY: needs_underlay=true; break;
        case KSN_PIXEL_SHR:
            if(ins->a>=KSN_PIXEL_REGS||!(written&(1u<<ins->a))||
               ins->immediate>15)return false;
            break;
        case KSN_PIXEL_ADD: case KSN_PIXEL_SUB: case KSN_PIXEL_MUL:
        case KSN_PIXEL_AND: case KSN_PIXEL_OR: case KSN_PIXEL_XOR:
            if(ins->a>=KSN_PIXEL_REGS||ins->b>=KSN_PIXEL_REGS||
               !(written&(1u<<ins->a))||!(written&(1u<<ins->b)))return false;
            break;
        default: return false;
        }
        written|=1u<<ins->dst;
    }
    if(!(written&(1u<<frame->color_reg))||
       !(written&(1u<<frame->alpha_reg)))return false;
    if(needs_underlay){
        if(!underlay||!underlay->rgb565||underlay->stride<frame->width||
           underlay->pixels<(size_t)(frame->height-1u)*underlay->stride+
                            frame->width)return false;
    }
    return true;
}

bool ksn_pixel_image_bind(ksn_pixel_image *image,const ksn_pixel_frame *frame,
                          const ksn_pixel_underlay *underlay){
    if(!image||!valid_frame(frame,underlay))return false;
    /* Kasane snapshots the port dimensions at registration. A bound image
     * keeps its geometry even when no presentation is in flight. */
    if(image->valid&&(image->frame.width!=frame->width||
                      image->frame.height!=frame->height))return false;
    image->frame=*frame;
    image->underlay=underlay?*underlay:(ksn_pixel_underlay){0};
    image->valid=true;
    image->cached=false;
    return true;
}

static void render_row(ksn_pixel_image *image,uint16_t y){
    const ksn_pixel_frame *f=&image->frame;
    for(unsigned x=0;x<f->width;x++){
        uint16_t reg[KSN_PIXEL_REGS]={0};
        for(unsigned i=0;i<f->count;i++){
            const ksn_pixel_instruction *ins=&f->code[i];
            uint16_t v=0;
            switch(ins->op){
            case KSN_PIXEL_IMM: v=ins->immediate; break;
            case KSN_PIXEL_X: v=(uint16_t)x; break;
            case KSN_PIXEL_Y: v=y; break;
            case KSN_PIXEL_PARAM: v=f->params[ins->immediate]; break;
            case KSN_PIXEL_UNDERLAY:
                v=image->underlay.rgb565[(size_t)y*image->underlay.stride+x];
                break;
            case KSN_PIXEL_ADD: v=(uint16_t)(reg[ins->a]+reg[ins->b]); break;
            case KSN_PIXEL_SUB: v=(uint16_t)(reg[ins->a]-reg[ins->b]); break;
            case KSN_PIXEL_MUL:
                v=(uint16_t)((uint32_t)reg[ins->a]*reg[ins->b]); break;
            case KSN_PIXEL_SHR: v=(uint16_t)(reg[ins->a]>>ins->immediate); break;
            case KSN_PIXEL_AND: v=(uint16_t)(reg[ins->a]&reg[ins->b]); break;
            case KSN_PIXEL_OR: v=(uint16_t)(reg[ins->a]|reg[ins->b]); break;
            case KSN_PIXEL_XOR: v=(uint16_t)(reg[ins->a]^reg[ins->b]); break;
            default: break; /* validated at bind */
            }
            reg[ins->dst]=v;
        }
        image->color[x]=reg[f->color_reg];
        image->alpha[x]=reg[f->alpha_reg]>255?255:(uint8_t)reg[f->alpha_reg];
    }
    image->cached_y=y;
    image->cached=true;
}

static ksn_result read_span(void *ctx,uint16_t variant,uint16_t frame,
                            uint16_t y,uint16_t x,uint16_t count,
                            uint16_t *rgb565,uint8_t *alpha){
    ksn_pixel_image *image=ctx;
    if(!image||!image->valid||variant||frame||y>=image->frame.height||
       x>image->frame.width||count>image->frame.width-x||
       (count&&(!rgb565||!alpha)))return KSN_INVALID;
    if(count){
        if(!image->cached||image->cached_y!=y)render_row(image,y);
        memcpy(rgb565,image->color+x,(size_t)count*sizeof *rgb565);
        memcpy(alpha,image->alpha+x,count);
    }
    return KSN_OK;
}

void ksn_pixel_image_port(ksn_pixel_image *image,ksn_image_port *out){
    if(!out)return;
    *out=image&&image->valid?(ksn_image_port){
        .ctx=image,.width=image->frame.width,.height=image->frame.height,
        .variants=1,.frames=1,.read_span=read_span,.opaque=false
    }:(ksn_image_port){0};
}

static bool same_handle(ksn_pixel_handle a,ksn_pixel_handle b){
    return a.generation&&a.generation==b.generation&&a.slot==b.slot;
}

/* Never reuse a handle across pool reinitialization. Exhaustion refuses a
 * new candidate instead of wrapping back to a stale ACK token. */
static uint64_t next_pixel_generation;

bool ksn_pixel_pool_init(ksn_pixel_pool *pool,uint16_t width,uint16_t height,
                         uint16_t *underlay_storage[KSN_PIXEL_SLOTS],
                         size_t pixels_per_buffer){
    if(!pool||!width||width>240||!height||height>135)return false;
    /* Distinct buffers matter when committed and candidate use underlays. */
    if(underlay_storage){
        size_t bytes=(size_t)width*height*sizeof(uint16_t);
        for(unsigned i=0;i<KSN_PIXEL_SLOTS;i++){
            if(!underlay_storage[i])continue;
            if(pixels_per_buffer<(size_t)width*height)return false;
            for(unsigned j=0;j<i;j++){
                if(!underlay_storage[j])continue;
                uintptr_t a=(uintptr_t)underlay_storage[i];
                uintptr_t b=(uintptr_t)underlay_storage[j];
                if(a<=b?b-a<bytes:a-b<bytes)return false;
            }
        }
    }
    memset(pool,0,sizeof *pool);
    pool->width=width;
    pool->height=height;
    pool->underlay_capacity=pixels_per_buffer;
    if(underlay_storage)
        for(unsigned i=0;i<KSN_PIXEL_SLOTS;i++)
            pool->slots[i].underlay_pixels=underlay_storage[i];
    return true;
}

ksn_pixel_result ksn_pixel_pool_stage(ksn_pixel_pool *pool,
                                      const ksn_pixel_frame *frame,
                                      const ksn_pixel_underlay *underlay,
                                      ksn_pixel_handle *handle){
    if(!pool||!handle||!pool->width||!frame||
       frame->width!=pool->width||frame->height!=pool->height||
       !valid_frame(frame,underlay))return KSN_PIXEL_INVALID;
    if(pool->pending.generation||pool->repair_required||
       next_pixel_generation==UINT64_MAX)return KSN_PIXEL_BUSY;
    bool needs_underlay=false;
    for(unsigned i=0;i<frame->count;i++)
        if(frame->code[i].op==KSN_PIXEL_UNDERLAY){needs_underlay=true;break;}
    int free_slot=-1;
    for(unsigned i=0;i<KSN_PIXEL_SLOTS;i++){
        if(same_handle(pool->committed,
                       (ksn_pixel_handle){pool->slots[i].generation,(uint8_t)i}))
            continue;
        if(needs_underlay&&(!pool->slots[i].underlay_pixels||
           pool->underlay_capacity<(size_t)pool->width*pool->height))continue;
        free_slot=(int)i;break;
    }
    if(free_slot<0)return KSN_PIXEL_BUSY;
    ksn_pixel_slot *slot=&pool->slots[free_slot];
    ksn_pixel_underlay snapshot={0};
    if(needs_underlay){
        /* A staging input must not alias pool storage: row copies could
         * otherwise overwrite future source rows or a committed underlay. */
        uintptr_t src=(uintptr_t)underlay->rgb565;
        size_t source_bytes=((size_t)(frame->height-1u)*underlay->stride+
                             frame->width)*sizeof(uint16_t);
        size_t dest_bytes=(size_t)pool->width*pool->height*sizeof(uint16_t);
        for(unsigned i=0;i<KSN_PIXEL_SLOTS;i++){
            if(!pool->slots[i].underlay_pixels)continue;
            uintptr_t dst=(uintptr_t)pool->slots[i].underlay_pixels;
            if(src<=dst?dst-src<source_bytes:src-dst<dest_bytes)
                return KSN_PIXEL_INVALID;
        }
        for(unsigned y=0;y<frame->height;y++)
            memcpy(slot->underlay_pixels+(size_t)y*pool->width,
                   underlay->rgb565+(size_t)y*underlay->stride,
                   (size_t)pool->width*sizeof(uint16_t));
        snapshot=(ksn_pixel_underlay){slot->underlay_pixels,
                     (size_t)pool->width*pool->height,pool->width};
    }
    if(!ksn_pixel_image_bind(&slot->image,frame,
                             needs_underlay?&snapshot:NULL))
        return KSN_PIXEL_INVALID;
    slot->generation=++next_pixel_generation;
    pool->pending=(ksn_pixel_handle){slot->generation,(uint8_t)free_slot};
    *handle=pool->pending;
    return KSN_PIXEL_OK;
}

bool ksn_pixel_pool_finish(ksn_pixel_pool *pool,ksn_pixel_handle handle,
                            bool transferred,bool previous_frame_retired){
    if(!pool||!same_handle(pool->pending,handle))return false;
    if(transferred){
        if(pool->committed.generation&&!previous_frame_retired)return false;
        pool->committed=handle;
        pool->repair_required=false;
    }else pool->repair_required=true;
    pool->pending=(ksn_pixel_handle){0};
    return true;
}

void ksn_pixel_pool_repair_done(ksn_pixel_pool *pool){
    if(pool&&!pool->pending.generation)pool->repair_required=false;
}

ksn_pixel_handle ksn_pixel_pool_committed(const ksn_pixel_pool *pool){
    return pool?pool->committed:(ksn_pixel_handle){0};
}

static ksn_result pool_span(void *ctx,uint16_t variant,uint16_t frame,
                             uint16_t y,uint16_t x,uint16_t count,
                             uint16_t *rgb565,uint8_t *alpha){
    ksn_pixel_pool *pool=ctx;
    if(!pool||variant||frame>=KSN_PIXEL_SLOTS)return KSN_INVALID;
    ksn_pixel_slot *slot=&pool->slots[frame];
    ksn_pixel_handle handle={slot->generation,(uint8_t)frame};
    if(!same_handle(handle,pool->committed)&&
       !same_handle(handle,pool->pending))return KSN_INVALID;
    return read_span(&slot->image,0,0,y,x,count,rgb565,alpha);
}

void ksn_pixel_pool_port(ksn_pixel_pool *pool,ksn_image_port *out){
    if(!out)return;
    *out=pool&&pool->width?(ksn_image_port){
        .ctx=pool,.width=pool->width,.height=pool->height,
        .variants=1,.frames=KSN_PIXEL_SLOTS,.read_span=pool_span,.opaque=false
    }:(ksn_image_port){0};
}
