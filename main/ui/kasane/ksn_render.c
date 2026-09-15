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
/* Rotated spans: step the source quotient instead of dividing per pixel.
 * U = u*source_width is affine in x, so with D = du*source_width written as
 * D = q*B + r (q = floor(D/B), 0 <= r < B) the floor/mod pair of the same
 * division advances by q + (rem+r >= B) and rem+r-(-B). That reproduces
 * floor(U/B) exactly for every pixel, so the source index and the block left
 * for the compositor are bit-identical to the per-pixel division. The range
 * test moves with it: 0 <= u < umax is exactly 0 <= sx < source_width because
 * rem is in [0,B). Only the span anchor (one pixel in sixteen) divides, with
 * the truncating quotient corrected to floor for negative numerators. */
bool g_ksn_image_rotate_step=true;
bool g_ksn_image_rotate_anchor=true;
bool g_ksn_image_rotate_reject=true;
/* One destination row of rotated-span anchors, built at run time in DRAM.
 * The per-span anchor costs the same two 64-bit divisions the scalar path
 * takes, but the anchors are affine in x: with the 16-pixel numerator step
 * D = 16*du*source_width written as D = q*B + r (0 <= r < B), floor(U/B) and
 * U mod B advance by the carry rule the per-pixel loop already uses. Entry j
 * is therefore *exactly* the quotient and remainder of the division at
 * x = base_x + 16j, so the renderer reads it instead of dividing. The table is
 * filled lazily as a row is read left to right. Nothing in it is constant, so
 * it costs SRAM and .text only: 0 bytes of flash (.rodata unchanged) and
 * sizeof(ksn_anchor_row) of .bss. */
#define KSN_ANCHOR_SPANS 16
typedef struct {
    int32_t rotation,bounds_x,bounds_y,window; /* affine inputs, packed pairwise */
    int32_t row;                               /* destination row of the entries */
    int32_t base_x;                            /* destination x of entry 0 */
    int32_t spans;                             /* entries built */
    int32_t um,vm,q16u,r16u,q16v,r16v;         /* 16-pixel increment pair */
    int32_t sx[KSN_ANCHOR_SPANS],remu[KSN_ANCHOR_SPANS]; /* 16 spans cover a 240-wide row */
    int32_t sy[KSN_ANCHOR_SPANS],remv[KSN_ANCHOR_SPANS];
} ksn_anchor_row;
static ksn_anchor_row g_anchor_row;
#ifdef KSN_ANCHOR_COUNT
uint32_t g_ksn_image_anchor_builds,g_ksn_image_reject_tests,g_ksn_image_reject_spans;
#endif
/* Entries after the first advance by the span step: one comparison and one
 * conditional subtraction per axis, the same rule as the per-pixel loop.
 * remu and r16u are both in [0,um), so the carry is at most one and the test
 * against um-r16u keeps every intermediate inside int32 (um itself can reach
 * 2^31, which is why the sum is never formed before the comparison). */
static void anchor_extend(ksn_anchor_row *t,int last){
    while(t->spans<=last){
        int i=t->spans,ux=t->sx[i-1],ru=t->remu[i-1],uy=t->sy[i-1],rv=t->remv[i-1];
        if(ru>=t->um-t->r16u){ru-=t->um-t->r16u;ux+=t->q16u+1;}else{ru+=t->r16u;ux+=t->q16u;}
        if(rv>=t->vm-t->r16v){rv-=t->vm-t->r16v;uy+=t->q16v+1;}else{rv+=t->r16v;uy+=t->q16v;}
        t->sx[i]=ux;t->remu[i]=ru;t->sy[i]=uy;t->remv[i]=rv;t->spans=i+1;
    }
}
/* Seed entry 0 with the exact division the caller computed for this pixel and
 * remember the affine inputs it came from (bounds packed pairwise so the
 * per-span check is four word compares). */
static void anchor_put(int32_t rotation,int32_t bx,int32_t by,int32_t window,int x,int y,
                       int step_u,int step_v,unsigned sw,unsigned sh,int um,int vm,
                       int sx,int remu,int sy,int remv){
    ksn_anchor_row *t=&g_anchor_row;
    int su=step_u*16*(int)sw,sv=step_v*16*(int)sh;
    int q16u=su/um,r16u=su-q16u*um,q16v=sv/vm,r16v=sv-q16v*vm;
    if(r16u<0){q16u--;r16u+=um;}                  /* truncating quotient -> floor */
    if(r16v<0){q16v--;r16v+=vm;}
    t->rotation=rotation;t->bounds_x=bx;t->bounds_y=by;t->window=window;t->row=y;
    t->base_x=x;t->spans=1;
    t->um=um;t->vm=vm;t->q16u=q16u;t->r16u=r16u;t->q16v=q16v;t->r16v=r16v;
    t->sx[0]=sx;t->remu[0]=remu;t->sy[0]=sy;t->remv[0]=remv;
#ifdef KSN_ANCHOR_COUNT
    g_ksn_image_anchor_builds++;
#endif
}
/* Host diagnostics: the entries the last rotated row built and the x they
 * start at. The renderer never reads this; it exists so a harness can assert
 * the table was actually served rather than rebuilt per span. */
uint32_t ksn_render_rotate_anchor_state(int *base_x){
    if(base_x)*base_x=g_anchor_row.base_x;
    return (uint32_t)g_anchor_row.spans;
}
static ksn_result image_read(ksn_core *core,ksn_tx ticket,ksn_layer layer,unsigned index,
                             const ksn_draw *d,int x,int y,unsigned *count,ksn_span_scratch *scratch){
    if(d->data.image.rotation){
        /* The affine numerator advances exactly by addition along a span.
         * Keep the original rational division/rounding at source lookup. */
        int c=ksn_image_sin(d->data.image.rotation+256),s=ksn_image_sin(d->data.image.rotation);
        int64_t w=(int)d->bounds.x1-d->bounds.x0,h=(int)d->bounds.y1-d->bounds.y0;
        int64_t umax=w*32768,vmax=h*32768;
        unsigned cached_y=UINT32_MAX,cached_x=UINT32_MAX;
        unsigned sw=d->data.image.source_width,sh=d->data.image.source_height;
        if(!g_ksn_image_rotate_step||!sw||!sh||!w||!h){
            /* Rational division at source lookup, once per destination pixel. */
            int64_t qx=2ll*x+1-d->bounds.x0-d->bounds.x1,qy=2ll*y+1-d->bounds.y0-d->bounds.y1;
            int64_t u=w*16384+qx*c+qy*s,v=h*16384-qx*s+qy*c;
            for(unsigned i=0;i<*count;i++,u+=2*c,v-=2*s){
                unsigned sx,sy;scratch->rotated.rgb[i]=0;scratch->rotated.alpha[i]=0;
                if(u<0||v<0||u>=umax||v>=vmax)continue;
                sx=d->data.image.source_x+(unsigned)(u*sw/umax);
                sy=d->data.image.source_y+(unsigned)(v*sh/vmax);
                unsigned bx=d->data.image.source_x+((sx-d->data.image.source_x)/16)*16;
                if(sy!=cached_y||bx!=cached_x){
                    unsigned n=d->data.image.source_x+sw-bx;if(n>16)n=16;
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
        /* Step the quotient and remainder of the same divisions. All of the
         * stepping state stays in 32 bits, which is provable here: the source
         * extent is at most 256 and the sine is Q14, so |2c*sw| is at most
         * 8,388,608 and |q*um| <= |2c*sw|; the denominators are w*32768 with
         * w <= 65535 (int16 bounds), so they fit int32; and the remainder is
         * compared against um-remu_step before adding, which keeps every
         * intermediate inside [0,um). Only the span anchor divides a 64-bit
         * numerator, once per axis, and takes the remainder back out with an
         * inline multiply instead of a second library call. */
        int step_u=2*c,step_v=-2*s;
        int su=step_u*(int)sw,sv=step_v*(int)sh;
        int um=(int)umax,vm=(int)vmax;
        int qu=su/um,remu_step=su-qu*um;
        int qv=sv/vm,remv_step=sv-qv*vm;
        if(remu_step<0){qu--;remu_step+=um;}          /* truncating quotient -> floor */
        if(remv_step<0){qv--;remv_step+=vm;}
        /* The anchors of this span. Both arms leave sx/sy/remu/remv as
         * floor(U/B) and U mod B, so the pixel loop below cannot tell them
         * apart: the table arm reads what the division arm would compute. */
        int sx,sy,remu,remv;
        bool served=false;
        int j=0;
        if(g_ksn_image_rotate_anchor){
            /* The row table: entries at x = base_x + 16j, so a match on the
             * command's affine inputs and the row plus a delta that is a
             * non-negative multiple of 16 below the table's reach is all that
             * has to be checked. Nothing else is evaluated when the switch is
             * off, so the per-span division arm keeps its own cost. */
            int32_t key_bx=(uint16_t)d->bounds.x0|((int32_t)(uint16_t)d->bounds.x1<<16);
            int32_t key_by=(uint16_t)d->bounds.y0|((int32_t)(uint16_t)d->bounds.y1<<16);
            unsigned delta=(unsigned)(x-g_anchor_row.base_x);
            if(g_anchor_row.rotation==(int32_t)d->data.image.rotation&&
               g_anchor_row.bounds_x==key_bx&&g_anchor_row.bounds_y==key_by&&
               g_anchor_row.window==(int32_t)(sw|(sh<<16))&&g_anchor_row.row==y&&
               delta<16u*KSN_ANCHOR_SPANS&&!(delta&15u)){
                j=(int)(delta>>4);
                if(j>=g_anchor_row.spans)anchor_extend(&g_anchor_row,j);
                sx=g_anchor_row.sx[j];remu=g_anchor_row.remu[j];
                sy=g_anchor_row.sy[j];remv=g_anchor_row.remv[j];
                served=true;
            }
        }
        if(!served){
            /* The exact per-span division, and the seed for a fresh row table:
             * the affine numerator is only needed here, so the served arm
             * never computes it. */
            int64_t qx=2ll*x+1-d->bounds.x0-d->bounds.x1,qy=2ll*y+1-d->bounds.y0-d->bounds.y1;
            int64_t u=w*16384+qx*c+qy*s,v=h*16384-qx*s+qy*c;
            int64_t U=u*(int64_t)sw,V=v*(int64_t)sh;
            sx=(int)(U/umax);sy=(int)(V/vmax);
            remu=(int)(U-(int64_t)sx*umax);remv=(int)(V-(int64_t)sy*vmax);
            if(remu<0){sx--;remu+=um;}
            if(remv<0){sy--;remv+=vm;}
            if(g_ksn_image_rotate_anchor)
                anchor_put((int32_t)d->data.image.rotation,
                           (uint16_t)d->bounds.x0|((int32_t)(uint16_t)d->bounds.x1<<16),
                           (uint16_t)d->bounds.y0|((int32_t)(uint16_t)d->bounds.y1<<16),
                           (int32_t)(sw|(sh<<16)),x,y,step_u,step_v,sw,sh,um,vm,sx,remu,sy,remv);
        }
        /* Whole-span rejection. A span whose first pixel is accepted has an
         * accepted pixel, so the interval test only has to run when the first
         * pixel is outside; and the anchors are monotone along the span (the
         * per-pixel quotient has the sign of step_u and step_v, the carry only
         * adds), so the extreme pixel on the failing side decides the whole
         * span: with the two anchors at x and x+count, "every pixel below
         * zero" is the maximum below zero and "every pixel at or above the
         * source extent" is the minimum, and each of the two is known from the
         * anchor on its side plus one subtraction of the per-pixel quotient.
         * The end anchors come from the next table entry when the span is 16
         * pixels wide (which the next span reads anyway), otherwise from one
         * span-wide carry step of the same increment. Nothing here can change
         * a pixel: the loop it replaces is the one that rejects them. */
        if(g_ksn_image_rotate_reject&&(sx<0||sy<0||sx>=(int)sw||sy>=(int)sh)){
            int ux_end,uy_end;
#ifdef KSN_ANCHOR_COUNT
            g_ksn_image_reject_tests++;
#endif
            if(served&&*count==16){
                /* The row table already carries the 16-pixel increment pair of
                 * this command's affine line, so the end anchor is one carry
                 * step of it: no second division and no new table entry. */
                ux_end=sx+g_anchor_row.q16u+(remu>=g_anchor_row.um-g_anchor_row.r16u);
                uy_end=sy+g_anchor_row.q16v+(remv>=g_anchor_row.vm-g_anchor_row.r16v);
            }else{
                int du=step_u*(int)*count*(int)sw,dv=step_v*(int)*count*(int)sh;
                int qu_n=du/um,ru_n=du-qu_n*um,qv_n=dv/vm,rv_n=dv-qv_n*vm;
                if(ru_n<0){qu_n--;ru_n+=um;}          /* truncating quotient -> floor */
                if(rv_n<0){qv_n--;rv_n+=vm;}
                ux_end=sx+qu_n+(remu>=um-ru_n);uy_end=sy+qv_n+(remv>=vm-rv_n);
            }
            /* The interval the span's anchors can take, as a min/max pair per
             * axis: increasing anchors have min at the first pixel and max at
             * the end anchor, decreasing ones the other way round (the end
             * anchor of a decreasing axis is its value one pixel past the
             * span, so the minimum back one pixel is one quotient above it).
             * The span is out when the interval misses [0, extent) on either
             * axis. */
            int lo_u,hi_u,lo_v,hi_v;
            if(step_u>0){lo_u=sx;hi_u=ux_end;}else{lo_u=ux_end-qu-1;hi_u=sx;}
            if(step_v>0){lo_v=sy;hi_v=uy_end;}else{lo_v=uy_end-qv-1;hi_v=sy;}
            if(hi_u<0||hi_v<0||lo_u>=(int)sw||lo_v>=(int)sh){
#ifdef KSN_ANCHOR_COUNT
                g_ksn_image_reject_spans++;
#endif
                /* Exactly what the loop's reject path writes for a pixel: the
                 * caller reads only the first count entries, and a zero alpha
                 * makes the composited result a no-op in both callers. */
                for(unsigned i=0;i<*count;i++){scratch->rotated.rgb[i]=0;scratch->rotated.alpha[i]=0;}
                return KSN_OK;
            }
        }
        for(unsigned i=0;i<*count;i++){
            scratch->rotated.rgb[i]=0;scratch->rotated.alpha[i]=0;
            if(sx>=0&&sy>=0&&sx<(int)sw&&sy<(int)sh){
                unsigned column=(unsigned)(sx/16)*16;
                unsigned bx=(unsigned)d->data.image.source_x+column;
                unsigned source=(unsigned)d->data.image.source_x+(unsigned)sx;
                unsigned row=(unsigned)d->data.image.source_y+(unsigned)sy;
                if(row!=cached_y||bx!=cached_x){
                    unsigned n=(unsigned)d->data.image.source_x+sw-bx;if(n>16)n=16;
                    ksn_result r=ksn_core_image_span(core,ticket,false,layer,(uint16_t)index,(uint16_t)row,(uint16_t)bx,
                        (uint16_t)n,scratch->rotated.block_rgb,scratch->rotated.block_alpha);
                    if(r!=KSN_OK)return r;
                    cached_y=row;cached_x=bx;
                }
                scratch->rotated.rgb[i]=scratch->rotated.block_rgb[source-bx];
                scratch->rotated.alpha[i]=scratch->rotated.block_alpha[source-bx];
            }
            if(remu>=um-remu_step){remu-=um-remu_step;sx+=qu+1;}else{remu+=remu_step;sx+=qu;}
            if(remv>=vm-remv_step){remv-=vm-remv_step;sy+=qv+1;}else{remv+=remv_step;sy+=qv;}
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
