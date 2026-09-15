#include "ksn_render.h"
#include "ksn_image_transform.h"
#include <string.h>
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(KSN_PIE_FILL_MODEL)
/* The caller seeds the first aligned destination pixel. Broadcasting from
 * the output avoids a stack vector or a persistent colour table. Owner task
 * only: PIE is coprocessor 3. blocks is positive and dst is 16-byte aligned. */
static void __attribute__((noinline)) fill_blocks(uint16_t *dst,unsigned blocks){
#ifdef KSN_PIE_FILL_MODEL
    uint16_t color=*dst;
    for(unsigned i=0;i<blocks*8;i++)dst[i]=color;
#else
    __asm__ volatile(
        "ee.vldbc.16 q0, %[dst]\n"
        "loopgtz %[blocks], 1f\n"
        "  ee.vst.128.ip q0, %[dst], 16\n"
        "1:\n"
        : [dst] "+&a"(dst)
        : [blocks] "a"(blocks)
        : "memory");
#endif
}
#endif

static void fill565(uint16_t *dst,unsigned count,uint16_t color){
#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(KSN_PIE_FILL_MODEL)
    /* PIE rounds the address down; peel up to seven pixels before using it. */
    while(count&&((uintptr_t)dst&15u)){*dst++=color;count--;}
    unsigned blocks=count/8;
    if(blocks){
        *dst=color;fill_blocks(dst,blocks);
        dst+=blocks*8;count%=8;
    }
#endif
    while(count--)*dst++=color;
}

/* The type is the format tag: these channels are premultiplied, never straight. */
typedef struct { uint8_t r,g,b,a; } ksn_premultiplied_rgba8;
/* One shared span scratch across normal/group paths. Together with the group
 * tile (256), dither bits (8), and a provider's 128-byte row: 488 <= 512 bytes. */
typedef union {
    uint8_t text[64];
    struct { uint16_t rgb[32];uint8_t alpha[32]; } image;
    struct { uint16_t rgb[16];uint8_t alpha[16];uint16_t block_rgb[16];uint8_t block_alpha[16]; } rotated;
} ksn_span_scratch;
_Static_assert(sizeof(ksn_span_scratch)+256+8+128<=512,"compositor/provider pixel scratch budget");
static ksn_rgba image_color(uint16_t rgb,uint8_t alpha){
    unsigned r=rgb>>11,g=(rgb>>5)&63,b=rgb&31;
    return ((r<<3|r>>2)<<24)|((g<<2|g>>4)<<16)|((b<<3|b>>2)<<8)|alpha;
}
/* At most 16 destination pixels -> at most 31 contiguous source pixels. Half
 * scale samples pixel centers (source 1,3,...), double scale replicates 2x2. */
static unsigned stretch_sample(unsigned offset,unsigned source,unsigned destination){
    /* Pixel centers, exact integer mapping. Source <=256 and dest <=65535. */
    return (offset*source+source/2)/destination;
}
static ksn_result image_read(ksn_core *core,ksn_tx ticket,ksn_layer layer,unsigned index,
                             const ksn_draw *d,int x,int y,unsigned *count,ksn_span_scratch *scratch){
    if(d->data.image.rotation){
        unsigned cached_y=UINT32_MAX,cached_x=UINT32_MAX;
        for(unsigned i=0;i<*count;i++){
            unsigned sx,sy;scratch->rotated.rgb[i]=0;scratch->rotated.alpha[i]=0;
            if(!ksn_image_sample(d,x+(int)i,y,&sx,&sy))continue;
            unsigned bx=d->data.image.source_x+((sx-d->data.image.source_x)/16)*16;
            if(sy!=cached_y||bx!=cached_x){
                unsigned n=d->data.image.source_x+d->data.image.source_width-bx;if(n>16)n=16;
                ksn_result r=ksn_core_image_span(core,ticket,false,layer,(uint16_t)index,(uint16_t)sy,(uint16_t)bx,
                    (uint16_t)n,scratch->rotated.block_rgb,scratch->rotated.block_alpha);
                if(r!=KSN_OK)return r;
                cached_y=sy;cached_x=bx;
            }
            scratch->rotated.rgb[i]=scratch->rotated.block_rgb[sx-bx];
            scratch->rotated.alpha[i]=scratch->rotated.block_alpha[sx-bx];
        }
        return KSN_OK;
    }
    unsigned dx=(unsigned)(x-d->bounds.x0),dy=(unsigned)(y-d->bounds.y0),n=*count;
    if(d->data.image.scale==KSN_IMAGE_2X){n=((dx&1u)+*count+1)/2;dx/=2;dy/=2;}
    else if(d->data.image.scale==KSN_IMAGE_HALF){n=2*(*count)-1;dx=2*dx+1;dy=2*dy+1;}
    else if(d->data.image.scale==KSN_IMAGE_STRETCH){
        unsigned width=(unsigned)(d->bounds.x1-d->bounds.x0),start=stretch_sample(dx,d->data.image.source_width,width);
        while(*count>1&&stretch_sample(dx+*count-1,d->data.image.source_width,width)-start>=32)(*count)--;
        n=stretch_sample(dx+*count-1,d->data.image.source_width,width)-start+1;dx=start;
        dy=stretch_sample(dy,d->data.image.source_height,(unsigned)(d->bounds.y1-d->bounds.y0));
    }
    return ksn_core_image_span(core,ticket,false,layer,(uint16_t)index,
        (uint16_t)(d->data.image.source_y+dy),(uint16_t)(d->data.image.source_x+dx),
        (uint16_t)n,scratch->image.rgb,scratch->image.alpha);
}
static unsigned image_sample_index(const ksn_draw *d,int x,unsigned offset){
    if(d->data.image.rotation)return offset;
    if(d->data.image.scale==KSN_IMAGE_STRETCH){
        unsigned dx=(unsigned)(x-d->bounds.x0),width=(unsigned)(d->bounds.x1-d->bounds.x0);
        return stretch_sample(dx+offset,d->data.image.source_width,width)-stretch_sample(dx,d->data.image.source_width,width);
    }
    if(d->data.image.scale==KSN_IMAGE_HALF)return 2*offset;
    if(d->data.image.scale==KSN_IMAGE_2X)return (((unsigned)(x-d->bounds.x0)&1u)+offset)/2;
    return offset;
}
static const uint8_t bayer4[4][4]={{0,8,2,10},{12,4,14,6},{3,11,1,9},{15,7,13,5}};
static ksn_rect footprint(const ksn_draw *d){
    return d->kind==KSN_IMAGE?ksn_image_footprint(d->bounds,d->data.image.rotation):d->bounds;
}
static ksn_rgba image_sample_color(const ksn_draw *d,const ksn_span_scratch *s,unsigned index){
    return d->data.image.rotation?image_color(s->rotated.rgb[index],s->rotated.alpha[index]):
                                  image_color(s->image.rgb[index],s->image.alpha[index]);
}
static unsigned mul8(unsigned a,unsigned b){return (a*b+127)/255;}
static unsigned clamp8(unsigned value){return value>255?255:value;}
static bool inside_round_rect(ksn_rect bounds,uint8_t radius,int x,int y){
    if(!radius)return true;
    int cx=x<bounds.x0+radius?bounds.x0+radius:
           x>=bounds.x1-radius?bounds.x1-radius:x;
    int cy=y<bounds.y0+radius?bounds.y0+radius:
           y>=bounds.y1-radius?bounds.y1-radius:y;
    int dx=2*x+1-2*cx,dy=2*y+1-2*cy,r=2*radius;
    return dx*dx+dy*dy<=r*r;
}
static bool covers(const ksn_frame_command *c,int x,int y){
    const ksn_draw *d=&c->draw;
    if(!c->visible||x<d->bounds.x0||x>=d->bounds.x1||y<d->bounds.y0||y>=d->bounds.y1||
       x<d->clip.x0||x>=d->clip.x1||y<d->clip.y0||y>=d->clip.y1)return false;
    switch(d->kind){
    case KSN_RECT:return true;
    case KSN_TEXT:return true;
    case KSN_ROUND_RECT:return inside_round_rect(d->bounds,d->data.shape.radius,x,y);
    case KSN_STROKE:{
        int width=d->data.shape.width;
        return x<d->bounds.x0+width||x>=d->bounds.x1-width||
               y<d->bounds.y0+width||y>=d->bounds.y1-width;
    }
    case KSN_GRADIENT:return inside_round_rect(d->bounds,d->data.gradient.radius,x,y);
    default:return false;
    }
}
static unsigned channel(ksn_rgba color,unsigned shift){return (color>>shift)&255u;}
static ksn_rgba interpolate(ksn_rgba from,ksn_rgba to,unsigned i,unsigned length){
    if(length<=1)return from;
    unsigned last=length-1,result=0;
    const unsigned shifts[4]={24,16,8,0};
    for(unsigned n=0;n<4;n++){
        unsigned value=(channel(from,shifts[n])*(last-i)+channel(to,shifts[n])*i+last/2)/last;
        result|=value<<shifts[n];
    }
    return result;
}
static ksn_rgba sample(const ksn_frame_command *command,int x,int y){
    const ksn_draw *draw=&command->draw;
    if(draw->kind==KSN_TEXT)return draw->data.text.color;
    if(draw->kind!=KSN_GRADIENT)return draw->data.shape.color;
    bool vertical=draw->data.gradient.axis!=0;
    unsigned length=(unsigned)(vertical?draw->bounds.y1-draw->bounds.y0:
                                        draw->bounds.x1-draw->bounds.x0);
    unsigned i=(unsigned)(vertical?y-draw->bounds.y0:x-draw->bounds.x0);
    return interpolate(draw->data.gradient.from,draw->data.gradient.to,i,length);
}
static unsigned premultiply_over(ksn_premultiplied_rgba8 *dst,ksn_rgba color,uint8_t opacity){
    unsigned a=mul8(color&255,opacity),inverse=255-a;
    dst->r=(uint8_t)clamp8(mul8(color>>24,a)+mul8(dst->r,inverse));
    dst->g=(uint8_t)clamp8(mul8((color>>16)&255,a)+mul8(dst->g,inverse));
    dst->b=(uint8_t)clamp8(mul8((color>>8)&255,a)+mul8(dst->b,inverse));
    dst->a=(uint8_t)clamp8(a+mul8(dst->a,inverse));
    return a;
}
static unsigned quantize(unsigned value,unsigned maximum,unsigned bayer){
    unsigned q=value*maximum/255u,remainder=value*maximum-255u*q;
    if(q<maximum&&32u*remainder>(2u*bayer+1u)*255u)q++;
    return q;
}
static uint16_t pack565(unsigned r,unsigned g,unsigned b,bool dither,int x,int y){
    if(!dither)return (uint16_t)((r>>3)<<11|(g>>2)<<5|(b>>3));
    unsigned threshold=bayer4[(unsigned)y&3u][(unsigned)x&3u];
    return (uint16_t)(quantize(r,31,threshold)<<11|
                      quantize(g,63,threshold)<<5|quantize(b,31,threshold));
}
static uint16_t group_over(uint16_t dst,ksn_premultiplied_rgba8 src,uint8_t opacity,
                           bool dither,int x,int y){
    unsigned alpha=mul8(src.a,opacity);
    if(!alpha)return dst;
    unsigned inverse=255-alpha;
    unsigned r=dst>>11,g=(dst>>5)&63,b=dst&31;
    r=clamp8(mul8(src.r,opacity)+mul8((r<<3)|(r>>2),inverse));
    g=clamp8(mul8(src.g,opacity)+mul8((g<<2)|(g>>4),inverse));
    b=clamp8(mul8(src.b,opacity)+mul8((b<<3)|(b>>2),inverse));
    return pack565(r,g,b,dither,x,y);
}
static ksn_result render_group(ksn_core *core,const ksn_text_port *text,ksn_span_scratch *scratch,ksn_tx ticket,ksn_layer layer,unsigned first,unsigned end,
                               uint8_t opacity,int y,int rows,uint16_t *pixels){
    if(!opacity)return KSN_OK;
    ksn_premultiplied_rgba8 tile[64]; /* 256 bytes; no full component surface. */
    ksn_frame_command command;
    bool has_dither=false;
    int left=240,right=0,top=y+rows,bottom=y;
    for(unsigned i=first;i<=end;i++){
        ksn_result result=ksn_core_read(core,ticket,false,layer,(uint16_t)i,&command);
        if(result!=KSN_OK)return result;
        const ksn_draw *d=&command.draw;
        if(!command.visible||!d->opacity)continue;
        ksn_rect bounds=footprint(d);
        int x0=bounds.x0>d->clip.x0?bounds.x0:d->clip.x0;
        int x1=bounds.x1<d->clip.x1?bounds.x1:d->clip.x1;
        int y0=bounds.y0>d->clip.y0?bounds.y0:d->clip.y0;
        int y1=bounds.y1<d->clip.y1?bounds.y1:d->clip.y1;
        if(x0>=x1||y0>=y1)continue;
        if(d->kind==KSN_GRADIENT&&d->data.gradient.dither)has_dither=true;
        if(x0<left)left=x0;
        if(x1>right)right=x1;
        if(y0<top)top=y0;
        if(y1>bottom)bottom=y1;
    }
    if(left<0)left=0;
    if(right>240)right=240;
    if(top<y)top=y;
    if(bottom>y+rows)bottom=y+rows;
    for(int py=top;py<bottom;py++)for(int x0=left;x0<right;x0+=64){
        int count=right-x0;if(count>64)count=64;
        memset(tile,0,sizeof(tile));
        /* Boolean provenance survives partial coverage but is replaced by an
         * opaque child. Two words avoid variable 64-bit shifts on ESP32-S3. */
        uint32_t dither_pixels[2]={0,0};
        for(unsigned i=first;i<=end;i++){
            ksn_result result=ksn_core_read(core,ticket,false,layer,(uint16_t)i,&command);
            if(result!=KSN_OK)return result;
            bool child_dither=command.draw.kind==KSN_GRADIENT&&command.draw.data.gradient.dither;
            const ksn_draw *d=&command.draw;
            ksn_rect bounds=footprint(d);
            if(!command.visible||!d->opacity||py<bounds.y0||py>=bounds.y1||
               py<d->clip.y0||py>=d->clip.y1||x0>=bounds.x1||x0>=d->clip.x1||
               x0+count<=bounds.x0||x0+count<=d->clip.x0)continue;
            if(d->kind==KSN_IMAGE){
                int left=x0,right=x0+count;
                if(left<bounds.x0)left=bounds.x0;
                if(left<d->clip.x0)left=d->clip.x0;
                if(right>bounds.x1)right=bounds.x1;
                if(right>d->clip.x1)right=d->clip.x1;
                for(int x=left;x<right;){
                    unsigned n=(unsigned)(right-x);if(n>16)n=16;
                    result=image_read(core,ticket,layer,i,d,x,py,&n,scratch);
                    if(result!=KSN_OK)return result;
                    for(unsigned j=0;j<n;j++){
                        unsigned source=image_sample_index(d,x,j),dest=(unsigned)(x-x0)+j;
                        unsigned alpha=premultiply_over(&tile[dest],image_sample_color(d,scratch,source),d->opacity);
                        if(has_dither&&alpha==255)dither_pixels[dest>>5]&=~(1u<<(dest&31));
                    }
                    x+=(int)n;
                }
                continue;
            }
            if(command.draw.kind==KSN_TEXT){
                result=text->span(text->ctx,&command.draw,command.reveal,x0,py,(unsigned)count,scratch->text);
                if(result!=KSN_OK)return result;
            }
            for(int x=0;x<count;x++)if(covers(&command,x0+x,py)){
                ksn_rgba color=sample(&command,x0+x,py);
                if(command.draw.kind==KSN_TEXT)color=(color&0xffffff00u)|mul8(color&255,scratch->text[x]);
                unsigned alpha=premultiply_over(&tile[x],color,command.draw.opacity);
                if(has_dither&&alpha){
                    uint32_t bit=1u<<((unsigned)x&31u);
                    if(child_dither)dither_pixels[(unsigned)x>>5]|=bit;
                    else if(alpha==255)dither_pixels[(unsigned)x>>5]&=~bit;
                }
            }
        }
        for(int x=0;x<count;x++){
            unsigned index=(unsigned)((py-y)*240+x0+x);
            bool dither=has_dither&&(dither_pixels[(unsigned)x>>5]&(1u<<((unsigned)x&31u)))!=0;
            pixels[index]=group_over(pixels[index],tile[x],opacity,dither,x0+x,py);
        }
    }
    return KSN_OK;
}

static uint16_t rgb565(ksn_rgba c){return (uint16_t)((c>>27)<<11|((c>>18)&63)<<5|((c>>11)&31));}
static uint16_t blend(uint16_t dst,ksn_rgba src,uint8_t opacity,bool dither,int x,int y){
    unsigned a=((src&255)*opacity+127)/255;
    if(!a)return dst;
    unsigned r=(dst>>11)&31,g=(dst>>5)&63,b=dst&31;
    r=(r<<3)|(r>>2);g=(g<<2)|(g>>4);b=(b<<3)|(b>>2);
    r=((src>>24)*a+r*(255-a)+127)/255;
    g=(((src>>16)&255)*a+g*(255-a)+127)/255;
    b=(((src>>8)&255)*a+b*(255-a)+127)/255;
    return pack565(r,g,b,dither,x,y);
}
ksn_result ksn_render_rects(ksn_core *core,const ksn_display_port *display,ksn_render_stats *stats){
    if(!core||!display||!stats||!display->strip||!display->present||
       display->width!=240||display->height!=135||display->strip_rows!=8)return KSN_INVALID;
    *stats=(ksn_render_stats){0};
    ksn_frame frame;ksn_result result=ksn_core_prepare_frame(core,&frame);
    if(result!=KSN_OK)return result;
    uint32_t mask;result=ksn_core_damage(core,frame.ticket,&mask);
    if(result!=KSN_OK){ksn_core_defer_repair(core,frame.ticket);return result;}
    if(!mask)return ksn_core_presented(core,frame.ticket);
    ksn_frame_command command;
    for(unsigned layer=0;layer<2;layer++)for(unsigned i=0;i<frame.next[layer].commands;i++){
        result=ksn_core_read(core,frame.ticket,false,(ksn_layer)layer,i,&command);
        if(result!=KSN_OK){ksn_core_defer_repair(core,frame.ticket);return result;}
        if(command.draw.kind<KSN_RECT||command.draw.kind>KSN_IMAGE){
            ksn_core_defer_repair(core,frame.ticket);return KSN_UNSUPPORTED;
        }
        if(command.draw.kind==KSN_TEXT){
            result=display->text&&display->text->span?
                display->text->span(display->text->ctx,&command.draw,command.reveal,0,0,0,NULL):KSN_UNSUPPORTED;
            if(result!=KSN_OK){ksn_core_defer_repair(core,frame.ticket);return result;}
        }
        if(command.draw.kind==KSN_IMAGE){
            result=ksn_core_image_span(core,frame.ticket,false,(ksn_layer)layer,i,0,0,0,NULL,NULL);
            if(result!=KSN_OK){ksn_core_defer_repair(core,frame.ticket);return result;}
        }
    }
    uint16_t *pixels=display->strip(display->ctx);
    if(!pixels){ksn_core_defer_repair(core,frame.ticket);return KSN_OOM;}
    ksn_span_scratch scratch;
    for(unsigned band=0;band<17;band++){
        if(!(mask&(1u<<band)))continue;
        int y=(int)band*8,rows=band==16?7:8;
        fill565(pixels,(unsigned)(240*rows),rgb565(frame.next_background));
        for(unsigned layer=0;layer<2;layer++)for(unsigned i=0;i<frame.next[layer].commands;i++){
            result=ksn_core_read(core,frame.ticket,false,(ksn_layer)layer,i,&command);
            if(result!=KSN_OK){ksn_core_failed(core,frame.ticket);return result;}
            if(command.group_begin){
                unsigned first=i;uint8_t opacity=command.group_opacity;
                while(!command.group_end){
                    if(++i>=frame.next[layer].commands){ksn_core_failed(core,frame.ticket);return KSN_INVALID;}
                    result=ksn_core_read(core,frame.ticket,false,(ksn_layer)layer,(uint16_t)i,&command);
                    if(result!=KSN_OK){ksn_core_failed(core,frame.ticket);return result;}
                }
                result=render_group(core,display->text,&scratch,frame.ticket,(ksn_layer)layer,first,i,opacity,y,rows,pixels);
                if(result!=KSN_OK){ksn_core_failed(core,frame.ticket);return result;}
                continue;
            }
            ksn_draw *d=&command.draw;
            if(!command.visible||!d->opacity)continue;
            ksn_rect bounds=footprint(d);
            int x0=bounds.x0,x1=bounds.x1,y0=bounds.y0,y1=bounds.y1;
            if(x0<d->clip.x0)x0=d->clip.x0;
            if(x1>d->clip.x1)x1=d->clip.x1;
            if(y0<d->clip.y0)y0=d->clip.y0;
            if(y1>d->clip.y1)y1=d->clip.y1;
            if(x0<0)x0=0;
            if(x1>240)x1=240;
            if(y0<y)y0=y;
            if(y1>y+rows)y1=y+rows;
            if(x0>=x1||y0>=y1)continue;
            if(d->kind==KSN_TEXT){
                for(int py=y0;py<y1;py++)for(int x=x0;x<x1;x+=64){
                    unsigned count=(unsigned)(x1-x);if(count>64)count=64;
                    result=display->text->span(display->text->ctx,d,command.reveal,x,py,count,scratch.text);
                    if(result!=KSN_OK){ksn_core_failed(core,frame.ticket);return result;}
                    for(unsigned i=0;i<count;i++)if(scratch.text[i]){
                        unsigned index=(unsigned)((py-y)*240+x)+i;
                        ksn_rgba color=(d->data.text.color&0xffffff00u)|mul8(d->data.text.color&255,scratch.text[i]);
                        pixels[index]=blend(pixels[index],color,d->opacity,false,x+(int)i,py);
                    }
                }
                continue;
            }
            if(d->kind==KSN_IMAGE){
                for(int py=y0;py<y1;py++)for(int x=x0;x<x1;){
                    unsigned count=(unsigned)(x1-x);if(count>16)count=16;
                    result=image_read(core,frame.ticket,(ksn_layer)layer,i,d,x,py,&count,&scratch);
                    if(result!=KSN_OK){ksn_core_failed(core,frame.ticket);return result;}
                    for(unsigned j=0;j<count;j++){
                        unsigned source=image_sample_index(d,x,j),index=(unsigned)((py-y)*240+x)+j;
                        pixels[index]=blend(pixels[index],image_sample_color(d,&scratch,source),
                                            d->opacity,false,x+(int)j,py);
                    }
                    x+=(int)count;
                }
                continue;
            }
            if(d->kind==KSN_RECT&&d->opacity==255&&(d->data.shape.color&255)==255){
                uint16_t color=rgb565(d->data.shape.color);
                for(int py=y0;py<y1;py++)
                    fill565(pixels+(py-y)*240+x0,(unsigned)(x1-x0),color);
                continue;
            }
            for(int py=y0;py<y1;py++)for(int x=x0;x<x1;x++)if(covers(&command,x,py)){
                unsigned index=(unsigned)((py-y)*240+x);
                pixels[index]=blend(pixels[index],sample(&command,x,py),d->opacity,
                                    d->kind==KSN_GRADIENT&&d->data.gradient.dither,x,py);
            }
        }
        result=display->present(display->ctx,(uint16_t)y,(uint16_t)rows,pixels);
        if(result!=KSN_OK){ksn_core_failed(core,frame.ticket);return result;}
        stats->bands|=1u<<band;stats->transferred_bytes+=(uint32_t)rows*240u*2u;
    }
    return ksn_core_presented(core,frame.ticket);
}
