#include "ksn_render.h"
#include "ksn_image_transform.h"
#include <string.h>
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#include "esp_cpu.h"
#endif

/* Boundary 7a-7b of docs/perf/kasane-opt-survey.md: the render path's own
 * counts. See ksn_render.h for what each field is and what it is not. */
int g_ksn_prof=0;
ksn_render_prof g_ksn_render_prof;
void ksn_render_prof_read(ksn_render_prof *out){
    *out=g_ksn_render_prof;
    g_ksn_render_prof=(ksn_render_prof){0};
}
unsigned ksn_render_band_count(uint32_t mask){
    unsigned count=0;
    while(mask){mask&=mask-1u;count++;}
    return count;
}
unsigned ksn_render_band_runs(uint32_t mask){
    unsigned runs=0;bool in_run=false;
    for(unsigned band=0;band<17;band++){
        bool set=((mask>>band)&1u)!=0;
        if(set&&!in_run)runs++;
        in_run=set;
    }
    return runs;
}
#ifdef ESP_PLATFORM
/* One cycle read. The fences keep the compiler from moving work across the
 * bracket, the same shape main/scene/garden.c and flower.c use; `rsr.ccount` is
 * one instruction, unlike esp_timer_get_time at 0.90 us a call. */
static uint32_t ksn_cycles(void){
    uint32_t cycles;
    __asm__ __volatile__("":::"memory");
    cycles=esp_cpu_get_cycle_count();
    __asm__ __volatile__("":::"memory");
    return cycles;
}
#else
/* No rsr.ccount without IDF headers, so the cycle columns stay 0 and only the
 * entry counts are available on host (docs/perf/pie-simd.md 6.7). */
static uint32_t ksn_cycles(void){return 0;}
#endif
/* The switch is cached per bracket: these sit in per-row and per-pixel loops,
 * and a global load inside a bracket would be part of what it measures. Each
 * BEGIN opens a block and declares its own ksn_t0, so nesting cannot collide. */
#define KSN_PROF_BEGIN() bool ksn_on=g_ksn_prof!=0;uint32_t ksn_t0=ksn_on?ksn_cycles():0u
#define KSN_PROF_END(field) do{if(ksn_on){g_ksn_render_prof.field##_cy+=ksn_cycles()-ksn_t0;g_ksn_render_prof.field##_n++;}}while(0)

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

/* Boundary 2a: a frame command is decoded once per frame, not once per band.
 * ksn_core_read costs 139 instructions plus memset(176)/memcpy and the band
 * loop asks for the same command once per band, twice per group child, so a
 * full frame decodes the same few commands hundreds of times
 * (docs/perf/kasane-opt-survey.md, boundary 2). This cache holds the fields the
 * band loop and the group composition read, plus the counted text bytes a
 * cached command needs to stay alive; no pointer into a command bank ever
 * escapes the borrowed view of ksn_core.h. The banks cannot change while a
 * frame is in flight: the renderer holds the sealed ticket, a guest cannot
 * start another builder before it is presented or discarded, and the port
 * contract already forbids a callback from mutating the core or reentering
 * presentation. Validity covers one ksn_render_rects call, so a retried frame
 * decodes again from scratch. Owner task only, like every entry point here. */
int g_ksn_decode_once=1;
typedef struct {
    ksn_draw draw;
    bool visible,group_begin,group_end;
    uint8_t group_opacity,reveal;
} ksn_frame_view;
static struct {
    ksn_frame_view view[KSN_COMMANDS];
    uint32_t valid[(KSN_COMMANDS+31u)/32u];
    char text[KSN_TEXT_BYTES];
    unsigned text_used;
    ksn_frame_command read; /* The ABI storage of the reference read path. */
} decoded;

/* The type is the format tag: these channels are premultiplied, never straight. */
typedef struct { uint8_t r,g,b,a; } ksn_premultiplied_rgba8;
/* One shared span scratch across normal/group paths. Together with the group
 * tile (256), dither bits (8), and a provider's 128-byte row: 504 <= 512 bytes. */
typedef union {
    uint8_t text[64];
    struct { uint16_t rgb[32];uint8_t alpha[32];uint8_t stretch[16]; } image;
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
/* Stretched spans: a destination column maps to the source index
 * floor((dx*source_width + source_width/2)/width), and the index used inside a
 * span is the difference of that quotient between the span's first column and
 * the pixel itself. The numerator advances by the constant source_width;
 * writing source_width = q*width + r (q = floor(source_width/width),
 * 0 <= r < width), the quotient and the remainder of the SAME division advance
 * by q + (rem+r >= width) and rem+r-(width if carried). rem and r are both in
 * [0,width), so the carry is at most one and the stepping reproduces the
 * per-pixel division exactly, not approximately. The span fills its indices
 * once (one comparison and one conditional subtraction a pixel, plus one
 * division a span for q and r) and the caller reads them back beside the pixel
 * it composites. Bounds: source_width <= 256 and width <= 65535, so the
 * numerator (<= 2^25) and every intermediate stay in 32 bits, and the span's
 * own 32-source-pixel clamp keeps an index under 32. */
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
bool g_ksn_image_stretch_step=true;
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
        unsigned source=d->data.image.source_width;
        unsigned width=(unsigned)(d->bounds.x1-d->bounds.x0);
        /* One numerator for both arms: the lookup divides it, the stepped arm
         * takes its quotient here and advances the remainder by addition. */
        unsigned numerator=dx*source+source/2,start=numerator/width,last=start;
        if(*count>1){
            last=stretch_sample(dx+*count-1,source,width);
            while(*count>1&&last-start>=32){
                (*count)--;
                last=stretch_sample(dx+*count-1,source,width);
            }
        }
        if(g_ksn_image_stretch_step){
            /* Source index of each destination column of this span, stepped. */
            unsigned q=source/width,r=source-q*width,rem=numerator-start*width,index=0;
            for(unsigned i=0;i<*count;i++){
                scratch->image.stretch[i]=(uint8_t)index;
                if(rem>=width-r){rem-=width-r;index+=q+1;}else{rem+=r;index+=q;}
            }
        }
        n=last-start+1;dx=start;
        dy=stretch_sample(dy,d->data.image.source_height,(unsigned)(d->bounds.y1-d->bounds.y0));
    }
    return ksn_core_image_span(core,ticket,false,layer,(uint16_t)index,
        (uint16_t)(d->data.image.source_y+dy),(uint16_t)(d->data.image.source_x+dx),
        (uint16_t)n,scratch->image.rgb,scratch->image.alpha);
}
static unsigned image_sample_index(const ksn_draw *d,const ksn_span_scratch *scratch,int x,unsigned offset){
    if(d->data.image.rotation)return offset;
    if(d->data.image.scale==KSN_IMAGE_STRETCH){
        /* The span stepped these indices; the division arm re-derives them. */
        if(g_ksn_image_stretch_step)return scratch->image.stretch[offset];
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
static bool covers(const ksn_frame_view *c,int x,int y){
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
/* TEMPORARY A/B switch for the row solver below: 1 = solve a row's covered x
 * set once per row per command (the shipping path), 0 = ask the predicate
 * above for every pixel, as this file has always done. Both arms have to run
 * in ONE binary: the same kernel moves 15% between builds from instruction
 * cache alignment alone (CLAUDE.md), and this change is smaller than that. */
int g_ksn_row_coverage = 1;

/* Coverage as per-row x intervals. `covers` above is the reference for the
 * pixel set; this solves the same set in closed form, so a row costs one solve
 * instead of one predicate call per pixel. Shapes are the ones `covers` names:
 * rect, text, stroke edges, and the round-rect/gradient arc (radius 0 is the
 * full box, as inside_round_rect returns true without looking at x). A row
 * never needs more than two runs: the two stroke edges, or the single interval
 * a rounded row always collapses to. Inside the clamp window the predicate's
 * dx is 1 (odd), so a covered pixel needs dd >= 1; with dd >= 1 the left arc,
 * the clamped middle and the right arc meet end to end, and when the two clamp
 * windows overlap (a box narrower than 2*radius) both arcs hang off the same
 * clamp point and still meet. */
#define KSN_ROW_RUNS 2
typedef struct { int x0,x1; } ksn_x_run; /* Half open, [x0,x1), like the bounds. */

/* Floor square root, bit by bit. The input is small (radius <= 255, so
 * dd <= 260,100) and this rounds down exactly, which is what the arc needs. */
static unsigned isqrt_u32(uint32_t value){
    uint32_t root=0,bit=1u<<30;
    while(bit>value)bit>>=2;
    while(bit){
        if(value>=root+bit){value-=root+bit;root=(root>>1)+bit;}
        else root>>=1;
        bit>>=2;
    }
    return (unsigned)root;
}
/* The arc row: pixel centres as inside_round_rect measures them. left/right
 * are already the caller's box and clip intersection. */
static unsigned round_rect_runs(ksn_rect bounds,unsigned radius,int y,int left,int right,
                                ksn_x_run *runs){
    int near=bounds.x0+(int)radius,far=bounds.x1-(int)radius;
    int cy=y<bounds.y0+(int)radius?bounds.y0+(int)radius:
           y>=bounds.y1-(int)radius?bounds.y1-(int)radius:y;
    int dy=2*y+1-2*cy,limit=2*(int)radius,remaining=limit*limit-dy*dy;
    if(remaining<1)return 0; /* dx is odd: dx*dx+dy*dy is at least 1+dy*dy. */
    unsigned reach=isqrt_u32((unsigned)remaining);
    int start=near-(int)((reach+1)/2);
    int end=far+(int)((reach-1)/2)+1;
    if(end<near)end=near; /* Overlapping clamp windows: the middle is empty. */
    if(start<left)start=left;
    if(end>right)end=right;
    if(start>=end)return 0;
    runs[0].x0=start;runs[0].x1=end;return 1;
}
/* Runs of x in [left,right) that `covers` would accept on row y. Returns 0, 1
 * or 2; the runs are ascending and disjoint, so a caller can composite them in
 * order exactly as the per-pixel loop would. Takes the same borrowed view the
 * predicate does after boundary 2a was integrated: the decoded fields are the
 * ones this solver reads, and the pixel set is unchanged. */
static unsigned coverage_runs(const ksn_frame_view *command,int y,int left,int right,
                              ksn_x_run *runs){
    const ksn_draw *d=&command->draw;
    if(!command->visible)return 0;
    if(left<d->bounds.x0)left=d->bounds.x0;
    if(left<d->clip.x0)left=d->clip.x0;
    if(right>d->bounds.x1)right=d->bounds.x1;
    if(right>d->clip.x1)right=d->clip.x1;
    if(left>=right)return 0;
    if(y<d->bounds.y0||y>=d->bounds.y1||y<d->clip.y0||y>=d->clip.y1)return 0;
    switch(d->kind){
    case KSN_RECT:case KSN_TEXT:
        runs[0].x0=left;runs[0].x1=right;return 1;
    case KSN_STROKE:{
        int width=d->data.shape.width;
        if(y<d->bounds.y0+width||y>=d->bounds.y1-width){
            runs[0].x0=left;runs[0].x1=right;return 1;
        }
        int near_end=d->bounds.x0+width;if(near_end>right)near_end=right;
        int far_start=d->bounds.x1-width;if(far_start<left)far_start=left;
        if(far_start<=near_end){ /* A stroke wide enough to meet itself. */
            runs[0].x0=left;runs[0].x1=right;return 1;
        }
        unsigned count=0;
        if(left<near_end){runs[count].x0=left;runs[count].x1=near_end;count++;}
        if(far_start<right){runs[count].x0=far_start;runs[count].x1=right;count++;}
        return count;
    }
    case KSN_ROUND_RECT:case KSN_GRADIENT:{
        unsigned radius=d->kind==KSN_GRADIENT?d->data.gradient.radius:d->data.shape.radius;
        if(!radius){runs[0].x0=left;runs[0].x1=right;return 1;}
        return round_rect_runs(d->bounds,radius,y,left,right,runs);
    }
    default:return 0;
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
static ksn_rgba sample(const ksn_frame_view *command,int x,int y){
    const ksn_draw *draw=&command->draw;
    if(draw->kind==KSN_TEXT)return draw->data.text.color;
    if(draw->kind!=KSN_GRADIENT)return draw->data.shape.color;
    bool vertical=draw->data.gradient.axis!=0;
    unsigned length=(unsigned)(vertical?draw->bounds.y1-draw->bounds.y0:
                                        draw->bounds.x1-draw->bounds.x0);
    unsigned i=(unsigned)(vertical?y-draw->bounds.y0:x-draw->bounds.x0);
    return interpolate(draw->data.gradient.from,draw->data.gradient.to,i,length);
}
/* Candidate 4c of docs/perf/kasane-opt-survey.md, generalised to a row profile:
 * the colour `sample` returns is a function of x alone for a horizontal
 * gradient, of y alone for a vertical one (one value for the whole row) and of
 * neither for the shape and text commands. Evaluating it per pixel pays an
 * out-of-line call (sample is 57 instructions at -Os) and, for a gradient, four
 * hardware divisions per pixel (the divider is 16-18 cycles,
 * docs/perf/pie-simd.md:233) for a value the row already knows. Build the row's
 * values once into a small table and the per-pixel work becomes a load.
 *
 * The ramp is bit-exact, not an approximation of the pixel path: it is the same
 * integer expression rearranged. `interpolate` computes, with a and b the two
 * channel bytes of from/to and last = length-1,
 *
 *     f(i) = (a*(last-i) + b*i + last/2) / last = (C + i*D) / last
 *     C = a*last + last/2,  D = b - a
 *
 * every operand of which is non-negative, so the truncating division it emits
 * is floor. With D = m*last + s (0 <= s < last) and i0 the row's first index,
 *
 *     f(i) = i*m + floor((C + i*s) / last) = i*m + q(i)
 *
 * and q moves by a carry of at most one per column, because r < last and
 * s < last give r + s < 2*last. So two divisions per channel set the step up
 * (m and the first index) and every further column is an add, a compare and a
 * conditional subtract: the divisions leave the pixel loop entirely and the
 * pixel loop is left with a load. test_row_table.c checks the two writings
 * agree over a sweep of channel pairs and lengths, and over 120 whole frames.
 *
 * Bounds: ksn_rect is int16_t, so length <= 65535 and last <= 65534, and the
 * window starts at most 33,008 columns into the ramp. D = b - a is within
 * +-255, and truncating division keeps |s| <= 255 and |m| <= 255/last, which
 * puts the numerator C + i0*s under 2^25 and every quotient in the byte range:
 * nothing here needs 64-bit arithmetic. */
int g_ksn_row_table=1;
#define KSN_ROW_TABLE_MAX 240 /* One panel row: the strip is 240 wide. */
typedef struct {
    bool constant;              /* One value for every x of the row. */
    uint32_t color;             /* That value, and the clamp for any other x. */
    int first;                  /* x of value[0] when not constant. */
    unsigned count;             /* Entries in value[] when not constant. */
    uint32_t value[KSN_ROW_TABLE_MAX];
} ksn_row_table;
/* One table for the whole renderer, not one per call site: the renderer is
 * owner-task only, the direct path's row loop and a group's child loop never
 * overlap, and the only code that runs between a build and its reads is the
 * composite itself (ksn_ports.h forbids a callback from re-entering the
 * renderer). Two tables would be 1,920 B of DRAM, one is 960 B: the integration
 * record prices boundary 2a's decode cache at 5,868 B of .bss against a DRAM
 * budget of ~334 KiB with 280,932 B idle free (CLAUDE.md, この機体で繰り返し踏む制約). */
static ksn_row_table row_table;
/* Floor division for a possibly negative numerator: the ramp's step is b - a
 * and can point down. The source makes two of these calls per channel per row
 * (m, and the first index's quotient); at -Os each becomes a `quos` and the
 * `m*last` subtraction next to it a `rems` of the same operands, so the divider
 * runs four times per channel, sixteen per row -- against the four per pixel
 * the pixel path spent (measured, see test_row_table.c's call counts and the
 * commit message's instruction accounting). */
static int floor_div(int numerator,int divisor){
    int quotient=numerator/divisor;
    if(numerator-quotient*divisor<0)quotient--;
    return quotient;
}
/* Fills `table` with the values `sample` returns on row y over the columns
 * [left,right) the caller's runs cover. False when this row is not one the
 * table covers, in which case the caller asks `sample` per pixel as before and
 * the pixels are the ones the old path produced. */
static bool row_table_build(ksn_row_table *table,const ksn_frame_view *command,
                            int y,int left,int right){
    const ksn_draw *draw=&command->draw;
    if(left>=right)return false;
    if(draw->kind!=KSN_GRADIENT){
        /* Every other supported kind returns one colour for every pixel: rect,
         * text, round rect, stroke, and whatever the union holds for a kind the
         * renderer rejects before its first transfer. */
        table->constant=true;
        table->color=draw->kind==KSN_TEXT?draw->data.text.color:draw->data.shape.color;
        table->count=0;
        return true;
    }
    const ksn_rgba from=draw->data.gradient.from,to=draw->data.gradient.to;
    unsigned length=(unsigned)(draw->data.gradient.axis!=0?draw->bounds.y1-draw->bounds.y0:
                                                           draw->bounds.x1-draw->bounds.x0);
    if(length<=1){ /* interpolate returns `from` without dividing. */
        table->constant=true;table->color=from;table->count=0;
        return true;
    }
    if(draw->data.gradient.axis!=0){
        int index=y-draw->bounds.y0;
        if(index<0||(unsigned)index>=length)return false; /* Not this row's box. */
        /* A vertical gradient's index is the row, so `sample` returns one
         * colour for the whole row; asking it once is the reference value by
         * definition, and it keeps `interpolate` to the call site it had. */
        table->constant=true;
        table->color=sample(command,left,y);
        table->count=0;
        return true;
    }
    unsigned last=length-1,index=(unsigned)(left-draw->bounds.x0),count=(unsigned)(right-left);
    if(left<draw->bounds.x0||count>KSN_ROW_TABLE_MAX)return false;
    if(index>last||index+count-1>last)return false; /* Asks for columns the ramp
                                                     * does not define; the
                                                     * covered window never
                                                     * does, but a caller that
                                                     * widened it must fall back. */
    const unsigned shifts[4]={24,16,8,0};
    int quotient[4],remainder[4],step[4],carry[4];
    for(unsigned n=0;n<4;n++){
        int a=(int)channel(from,shifts[n]),b=(int)channel(to,shifts[n]);
        int numerator=a*(int)last+(int)(last/2),delta=b-a;
        /* D = m*last + s by TRUNCATING division, not floor: the step s then
         * keeps D's sign and |s| <= min(255, last-1), which is what holds the
         * numerator below 2^25. A floor would give s close to `last` for a
         * descending ramp and overflow the numerator of a window far into a
         * long one (measured: alpha 255->0, last 65534, i0 32768, which produced
         * 0xffff807d instead of 0x8800807f before this line used `/`). The floor
         * the ramp needs is applied once, to the first index, where the quotient
         * can be negative. */
        int m=delta/(int)last,s=delta-m*(int)last;
        int first=numerator+(int)index*s;
        int q=floor_div(first,(int)last),r=first-q*(int)last;
        quotient[n]=(int)index*m+q;step[n]=m;remainder[n]=r;carry[n]=s;
    }
    /* The four channels step in lockstep, one byte each, in scalars rather than
     * through the arrays above: the measured column loop was 105 instructions
     * (-Os, xtensa-esp32s3-elf-objdump) when the state was indexed, against the
     * 57-instruction `sample` call this build replaces. Indexing the state
     * costs a load and a store per channel per column and turns each channel's
     * shift into a variable one. One carry and no borrow per column is enough
     * there: r is in [0,last) and |s| < last. */
    int q0=quotient[0],q1=quotient[1],q2=quotient[2],q3=quotient[3];
    int r0=remainder[0],r1=remainder[1],r2=remainder[2],r3=remainder[3];
    int m0=step[0],m1=step[1],m2=step[2],m3=step[3];
    int s0=carry[0],s1=carry[1],s2=carry[2],s3=carry[3];
    int limit=(int)last;
    for(unsigned i=0;i<count;i++){
        table->value[i]=(uint32_t)q0<<24|(uint32_t)q1<<16|(uint32_t)q2<<8|(uint32_t)q3;
        q0+=m0;r0+=s0;if(r0>=limit){r0-=limit;q0++;}else if(r0<0){r0+=limit;q0--;}
        q1+=m1;r1+=s1;if(r1>=limit){r1-=limit;q1++;}else if(r1<0){r1+=limit;q1--;}
        q2+=m2;r2+=s2;if(r2>=limit){r2-=limit;q2++;}else if(r2<0){r2+=limit;q2--;}
        q3+=m3;r3+=s3;if(r3>=limit){r3-=limit;q3++;}else if(r3<0){r3+=limit;q3--;}
    }
    table->constant=false;table->first=left;table->count=count;
    table->color=table->value[0]; /* The clamp value, so it is never unset. */
    return true;
}
/* The value `sample` would return for x. The callers ask only for the x their
 * runs cover, which is inside the window the table was built for; the clamp
 * keeps the read defined and in range if that ever stops being true. Inline,
 * because this is the per-pixel work the table exists to make a load: an
 * out-of-line call here would carry a call8, the argument moves and the result
 * move into every pixel. */
static inline __attribute__((always_inline))
ksn_rgba row_table_value(const ksn_row_table *table,int x){
    if(table->constant)return (ksn_rgba)table->color;
    unsigned index=(unsigned)(x-table->first);
    return (ksn_rgba)table->value[index<table->count?index:0];
}
/* The row's values, or NULL when the caller has to ask `sample` per pixel: the
 * switch is off, or this row is not one the builder covers (an empty window, a
 * window outside the command's own index range). */
static const ksn_row_table *row_table_for(const ksn_frame_view *command,int y,int left,int right){
    if(!g_ksn_row_table)return NULL;
    return row_table_build(&row_table,command,y,left,right)?&row_table:NULL;
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
/* One decode, shared by both paths: the cache stores it for the frame, the
 * reference path stores it for the next read only. */
static void decode_view(ksn_frame_view *view,const ksn_frame_command *command){
    view->draw=command->draw;
    view->visible=command->visible;
    view->group_begin=command->group_begin;
    view->group_end=command->group_end;
    view->group_opacity=command->group_opacity;
    view->reveal=command->reveal;
}
static ksn_frame_view *view_slot(unsigned slot){
    /* The core bounds the index, the clamp only keeps the name total. */
    return &decoded.view[slot<KSN_COMMANDS?slot:0];
}
/* False when this command's text does not fit the frame pool; the caller then
 * falls back to the reference read. The bank's own text pool is the same
 * total, so a full frame pool cannot happen in practice. */
static bool cache_view(unsigned slot,const ksn_frame_command *command){
    ksn_frame_view *view=view_slot(slot);
    decode_view(view,command);
    if(view->draw.kind==KSN_TEXT){
        unsigned bytes=view->draw.data.text.bytes;
        if(bytes>sizeof(decoded.text)-decoded.text_used)return false;
        memcpy(decoded.text+decoded.text_used,command->text,bytes);
        view->draw.data.text.utf8=bytes?decoded.text+decoded.text_used:decoded.text;
        decoded.text_used+=bytes;
    }
    decoded.valid[slot>>5]|=1u<<(slot&31u);
    return true;
}
/* previous=false only: the renderer never reads the displayed bank. */
static ksn_result frame_command(ksn_core *core,ksn_tx ticket,ksn_layer layer,uint16_t index,
                                const ksn_frame_view **out){
    unsigned slot=(layer==KSN_APP?0u:KSN_APP_COMMANDS)+index;
    if(g_ksn_decode_once&&slot<KSN_COMMANDS&&(decoded.valid[slot>>5]&(1u<<(slot&31u)))){
        *out=view_slot(slot);return KSN_OK;
    }
    ksn_result result=ksn_core_read(core,ticket,false,layer,index,&decoded.read);
    if(result!=KSN_OK)return result;
    if(g_ksn_decode_once&&slot<KSN_COMMANDS&&cache_view(slot,&decoded.read)){
        *out=view_slot(slot);return KSN_OK;
    }
    decode_view(view_slot(slot),&decoded.read);*out=view_slot(slot);
    return KSN_OK;
}
/* ---- tile-level switches and instrument (docs/perf/kasane-tile.md) -------- */
/* A group's children are composited into a 64-pixel scratch tile, block by
 * block. These three switches change how that block is walked. Each keeps the
 * old path reachable and is read once per render, so the arms of a same-binary
 * A/B differ only in the technique. Sizes, error bounds and measured numbers are
 * in docs/perf/kasane-tile.md. */
int g_ksn_tile_pixels=64;   /* the tile's block: 64 or 16 pixels */
int g_ksn_tile_reach=1;     /* exact (default): a child's loop runs over its own
                             * clipped x interval, and a block no child can
                             * reach is skipped whole -- no clear, no read, no
                             * drop */
int g_ksn_tile_smooth=1;    /* smooth layers: one exact anchor per block plus a
                             * per-pixel increment instead of a per-pixel sample.
                             * 1 = the exact remainder stepping (bit-identical to
                             * the chain, default), 2 = the 8.16 approximate
                             * increment (off by default: it moves pixels), 0 =
                             * the per-pixel chain */
#ifdef KSN_TILE_COUNT
/* Host-only: compiled out of every production build (-DKSN_TILE_COUNT, set by
 * tools/kasane_contract/run_group_tile.sh). "covered" is a tile pixel whose
 * accumulated alpha is non-zero, i.e. one a child actually wrote into. */
uint32_t ksn_tile_visited,ksn_tile_covered,ksn_tile_blocks,ksn_tile_skipped,
         ksn_tile_smooth_blocks,ksn_tile_smooth_pixels,ksn_tile_child_pixels;
#endif
/* Objdump can only attribute a per-pixel cost to a function that is not inlined
 * away. The measurement build (-DKSN_TILE_MEASURE, tools/kasane_contract/
 * run_group_tile.sh) keeps the tile's per-pixel steps out of line so their
 * instruction counts are read off the object; the production build lets the
 * compiler inline them exactly as before. The attribute changes no pixel. */
#ifdef KSN_TILE_MEASURE
#define KSN_TILE_MEASURED __attribute__((noinline))
#else
#define KSN_TILE_MEASURED
#endif
/* A group with more children than this falls back to the unconditional tile:
 * the reach table exists to skip work, never to decide which pixels are drawn. */
#define KSN_TILE_REACH_BOXES 16
typedef struct { int16_t x0,y0,x1,y1; } ksn_tile_reach;
static bool tile_block_reached(const ksn_tile_reach *box,unsigned count,
                               int x0,int width,int py){
    for(unsigned k=0;k<count;k++)
        if(py>=box[k].y0&&py<box[k].y1&&x0<box[k].x1&&x0+width>box[k].x0)return true;
    return false;
}
/* One covered pixel of one child: composite the colour the caller produced into
 * the premultiplied tile and maintain the dither provenance. Both coverage arms
 * (g_ksn_row_coverage on = the row's runs, off = the per-pixel predicate) call
 * this, so the pixels a row's runs name go through the same code the
 * pixel-at-a-time path runs. Out of line so that objdump can attribute the
 * tile's per-pixel cost (the doc counts it). `dest` is the pixel's index inside
 * the tile -- the index the dither provenance bit keys off -- and `coverage` is
 * the text port's ink at that pixel. Owner task: the decoded frame view, like
 * every other helper in this renderer (2a retype).
 *
 * `color` is an argument rather than a `sample(command,x,y)` call in here:
 * candidate 4c's row table (g_ksn_row_table) hands this the row's value and
 * must not re-enter `sample` per pixel -- its counting arm asserts the per-pixel
 * sampling is gone (test_row_table.c). Callers without a table pass
 * `sample(command,px,py)`, the same call with the same arguments this function
 * used to make itself, which is why merging the tile branch's composite and the
 * rowtable branch's copy into this one definition moved no pixel. */
KSN_TILE_MEASURED static void group_pixel(ksn_premultiplied_rgba8 *tile,const ksn_frame_view *command,
                        int dest,bool has_dither,bool child_dither,
                        uint32_t *provenance,uint8_t coverage,ksn_rgba color){
    if(command->draw.kind==KSN_TEXT)color=(color&0xffffff00u)|mul8(color&255,coverage);
    unsigned alpha=premultiply_over(&tile[dest],color,command->draw.opacity);
    if(has_dither&&alpha){
        uint32_t bit=1u<<((unsigned)dest&31u);
        if(child_dither)provenance[(unsigned)dest>>5]|=bit;
        else if(alpha==255)provenance[(unsigned)dest>>5]&=~bit;
    }
}
/* A smooth layer varies slowly across the block. A gradient is affine in its
 * axis, and with radius 0 `covers` is true over its whole clipped box, so the
 * run can be produced from one exact anchor instead of a sample per pixel. Two
 * ways, chosen by g_ksn_tile_smooth:
 *
 *   1 (exact): the value steps by the chain's own rational slope,
 *     D/last = q + rs/last with q = floor(D/last) and rs = D - q*last in
 *     [0,last). Stepping the remainder by rs and the value by q (carrying when
 *     the remainder reaches last) reproduces the quotient of
 *     from*last + i*D + last/2 by last at every pixel, i.e. exactly what
 *     `sample`'s interpolate returns -- no clamp, no drift, bit-identical.
 *   2 (approximate): one 8.16 increment taken from the run's two exact end
 *     values, truncated towards zero, so |offset| <= |span|<<16 at every pixel
 *     and the value cannot leave the closed interval the two ends span. What is
 *     left is the quantised increment; the doc measures it (at most one 8-bit
 *     level of deviation, so a fraction of a percent of covered pixels move by
 *     one 5/6/5 level) and it is off by default.
 *
 * A child that cannot vary along x -- a vertical gradient, or a length of one --
 * is a constant run under both modes, which is exact. */
KSN_TILE_MEASURED static void smooth_chord_block(ksn_premultiplied_rgba8 *tile,const ksn_frame_view *command,
                               int dest0,int count,ksn_rgba anchor,ksn_rgba end,uint8_t opacity,
                               bool has_dither,bool child_dither,uint32_t *provenance){
    (void)command;
    int32_t step[4];unsigned base[4];
    for(unsigned c=0;c<4;c++){
        unsigned shift=24-8*c;
        int32_t span=(int32_t)(((end>>shift)&255u)-((anchor>>shift)&255u));
        step[c]=count>1?(int32_t)(((int64_t)span<<16)/(count-1)):0;
        base[c]=(anchor>>shift)&255u;
    }
    int32_t offset[4]={0,0,0,0};
    for(int j=0;j<count;j++){
        ksn_rgba color=0;
        for(unsigned c=0;c<4;c++){
            color|=(ksn_rgba)((int)base[c]+((offset[c]+32768)>>16))<<(24-8*c);
            offset[c]+=step[c];
        }
        unsigned alpha=premultiply_over(&tile[dest0+j],color,opacity);
        if(has_dither&&alpha){
            uint32_t bit=1u<<((unsigned)(dest0+j)&31u);
            if(child_dither)provenance[(unsigned)(dest0+j)>>5]|=bit;
            else if(alpha==255)provenance[(unsigned)(dest0+j)>>5]&=~bit;
        }
    }
}
KSN_TILE_MEASURED static void smooth_exact_block(ksn_premultiplied_rgba8 *tile,const ksn_draw *d,ksn_rgba anchor,
                               int dest0,int count,int i0,int32_t last,bool has_dither,
                               bool child_dither,uint32_t *provenance){
    int32_t quotient[4],remainder_step[4],rem[4],offset[4];
    unsigned base[4];
    for(unsigned c=0;c<4;c++){
        unsigned shift=24-8*c;
        int32_t first=(int32_t)((d->data.gradient.from>>shift)&255u);
        int32_t span=(int32_t)((d->data.gradient.to>>shift)&255u)-first;
        int32_t q=span/last,rs=span-q*last;
        if(rs<0){q--;rs+=last;}                       /* q = floor(span/last) */
        base[c]=(anchor>>shift)&255u;
        quotient[c]=q;remainder_step[c]=rs;offset[c]=0;
        rem[c]=first*last+i0*span+last/2-(int32_t)base[c]*last;  /* in [0,last) */
    }
    for(int j=0;j<count;j++){
        ksn_rgba color=0;
        for(unsigned c=0;c<4;c++){
            color|=(ksn_rgba)((int)base[c]+offset[c])<<(24-8*c);
            rem[c]+=remainder_step[c];offset[c]+=quotient[c];
            if(rem[c]>=last){rem[c]-=last;offset[c]++;}
        }
        unsigned alpha=premultiply_over(&tile[dest0+j],color,d->opacity);
        if(has_dither&&alpha){
            uint32_t bit=1u<<((unsigned)(dest0+j)&31u);
            if(child_dither)provenance[(unsigned)(dest0+j)>>5]|=bit;
            else if(alpha==255)provenance[(unsigned)(dest0+j)>>5]&=~bit;
        }
    }
}
/* The dispatcher: one sample for the anchor both ways start from, then the arm
 * g_ksn_tile_smooth names. Kept small so it inlines into render_group and the two
 * bodies above stay out of line, which is what makes their objdump counts the
 * tile's per-pixel cost rather than a guess. */
KSN_TILE_MEASURED static void smooth_block(ksn_premultiplied_rgba8 *tile,const ksn_frame_view *command,
                         int dest0,int count,int x0,int y,bool has_dither,
                         bool child_dither,uint32_t *provenance){
    const ksn_draw *d=&command->draw;
    ksn_rgba anchor=sample(command,x0,y);
    int32_t last=(int32_t)(d->data.gradient.axis?d->bounds.y1-d->bounds.y0:d->bounds.x1-d->bounds.x0)-1;
    if(g_ksn_tile_smooth>=2||d->data.gradient.axis||last<1||count<2){
        ksn_rgba end=d->data.gradient.axis||last<1?anchor:(count>1?sample(command,x0+count-1,y):anchor);
        smooth_chord_block(tile,command,dest0,count,anchor,end,d->opacity,has_dither,
                           child_dither,provenance);
        return;
    }
    smooth_exact_block(tile,d,anchor,dest0,count,x0-d->bounds.x0,last,has_dither,
                       child_dither,provenance);
}
/* The tile's last step: group opacity, the premultiplied drop into RGB565 and
 * the dither provenance. Out of line for the same reason as group_pixel. */
KSN_TILE_MEASURED static void tile_row_over(uint16_t *pixels,const ksn_premultiplied_rgba8 *tile,int count,
                          uint8_t opacity,bool has_dither,const uint32_t *provenance,
                          int x0,int y,int py){
    for(int x=0;x<count;x++){
        unsigned index=(unsigned)((py-y)*240+x0+x);
        bool dither=has_dither&&(provenance[(unsigned)x>>5]&(1u<<((unsigned)x&31u)))!=0;
        pixels[index]=group_over(pixels[index],tile[x],opacity,dither,x0+x,py);
    }
}
static ksn_result render_group(ksn_core *core,const ksn_text_port *text,ksn_span_scratch *scratch,ksn_tx ticket,ksn_layer layer,unsigned first,unsigned end,
                               uint8_t opacity,int y,int rows,uint16_t *pixels){
    if(!opacity)return KSN_OK;
    ksn_premultiplied_rgba8 tile[64]; /* 256 bytes; no full component surface. */
    /* 2a's borrowed view and design-contracts' shared scratch in one body: the
     * decoded form stays `const ksn_frame_view *`, and the span coverage word
     * is scratch->text rather than a local (the 512-byte scratch budget). */
    const ksn_frame_view *command;
    bool has_dither=false;
    int left=240,right=0,top=y+rows,bottom=y;
    /* The reach table holds the clipped box of each child of this group. The
     * bounds pass already has every child in hand, so filling it costs no extra
     * ksn_core_read; testing the blocks against the children instead would cost
     * one per child per block (139 instructions plus a 176-byte clear each). */
    ksn_tile_reach reach[KSN_TILE_REACH_BOXES];
    unsigned reach_count=0;
    bool reach_all=true;
    for(unsigned i=first;i<=end;i++){
        ksn_result result;
        {KSN_PROF_BEGIN();
        result=frame_command(core,ticket,layer,(uint16_t)i,&command);
        KSN_PROF_END(read);}
        if(result!=KSN_OK)return result;
        const ksn_draw *d=&command->draw;
        ksn_tile_reach box={0,0,0,0};
        if(command->visible&&d->opacity){
            ksn_rect bounds=footprint(d);
            int x0=bounds.x0>d->clip.x0?bounds.x0:d->clip.x0;
            int x1=bounds.x1<d->clip.x1?bounds.x1:d->clip.x1;
            int y0=bounds.y0>d->clip.y0?bounds.y0:d->clip.y0;
            int y1=bounds.y1<d->clip.y1?bounds.y1:d->clip.y1;
            if(x0<x1&&y0<y1){
                /* The reach table caches exactly the clipped box this pass
                 * already computed, so a block no child overlaps can be skipped
                 * without re-reading a child (see the comment at `reach`). */
                box=(ksn_tile_reach){(int16_t)x0,(int16_t)y0,(int16_t)x1,(int16_t)y1};
                if(d->kind==KSN_GRADIENT&&d->data.gradient.dither)has_dither=true;
                if(x0<left)left=x0;
                if(x1>right)right=x1;
                if(y0<top)top=y0;
                if(y1>bottom)bottom=y1;
            }
        }
        if(i-first<KSN_TILE_REACH_BOXES)reach[i-first]=box;else reach_all=false;
        reach_count++;
    }
    if(left<0)left=0;
    if(right>240)right=240;
    if(top<y)top=y;
    if(bottom>y+rows)bottom=y+rows;
    int tile_pixels=g_ksn_tile_pixels>0&&g_ksn_tile_pixels<=64?g_ksn_tile_pixels:64;
    for(int py=top;py<bottom;py++)for(int x0=left;x0<right;x0+=tile_pixels){
        int count=right-x0;if(count>tile_pixels)count=tile_pixels;
        if(g_ksn_tile_reach&&reach_all&&!tile_block_reached(reach,reach_count,x0,count,py)){
#ifdef KSN_TILE_COUNT
            ksn_tile_skipped++;
#endif
            continue;
        }
#ifdef KSN_TILE_COUNT
        ksn_tile_blocks++;
#endif
        memset(tile,0,sizeof(tile));
        /* Boolean provenance survives partial coverage but is replaced by an
         * opaque child. Two words avoid variable 64-bit shifts on ESP32-S3. */
        uint32_t dither_pixels[2]={0,0};
        for(unsigned i=first;i<=end;i++){
            ksn_result result;
            {KSN_PROF_BEGIN();
            result=frame_command(core,ticket,layer,(uint16_t)i,&command);
            KSN_PROF_END(read);}
            if(result!=KSN_OK)return result;
            bool child_dither=command->draw.kind==KSN_GRADIENT&&command->draw.data.gradient.dither;
            const ksn_draw *d=&command->draw;
            ksn_rect bounds=footprint(d);
            if(!command->visible||!d->opacity||py<bounds.y0||py>=bounds.y1||
               py<d->clip.y0||py>=d->clip.y1)continue;
            /* The child's own window inside this block: outside it `covers` is
             * false everywhere, so the generic loop must not visit those pixels
             * at all. Off, the loop runs the whole block as before. The smooth
             * block below needs the same window whether or not the reach switch
             * is on -- it has no per-pixel `covers` to reject a stray column. */
            int lo=0,hi=count;
            {
                int cx0=bounds.x0>d->clip.x0?bounds.x0:d->clip.x0;
                int cx1=bounds.x1<d->clip.x1?bounds.x1:d->clip.x1;
                /* Block-level rejection: a child whose clipped box misses this
                 * block entirely draws nothing here, so its read, its span and
                 * its composite are all skipped. The window below is the same
                 * test per child, one sub-block at a time. */
                if(x0>=cx1||x0+count<=cx0)continue;
                if(g_ksn_tile_reach||g_ksn_tile_smooth){
                    lo=cx0-x0;if(lo<0)lo=0;
                    hi=cx1-x0;if(hi>count)hi=count;
                    if(lo>=hi)continue;
                }
            }
#ifdef KSN_TILE_COUNT
            ksn_tile_child_pixels+=(uint32_t)(hi-lo);
#endif
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
                        unsigned source=image_sample_index(d,scratch,x,j),dest=(unsigned)(x-x0)+j;
                        unsigned alpha=premultiply_over(&tile[dest],image_sample_color(d,scratch,source),d->opacity);
                        if(has_dither&&alpha==255)dither_pixels[dest>>5]&=~(1u<<(dest&31));
                    }
                    x+=(int)n;
                }
                continue;
            }
            if(command->draw.kind==KSN_TEXT){
                {KSN_PROF_BEGIN();
                result=text->span(text->ctx,&command->draw,command->reveal,x0,py,(unsigned)count,scratch->text);
                KSN_PROF_END(span);}
                if(result!=KSN_OK)return result;
            }
            /* A radius-0 gradient covers lo..hi exactly, so its value alone has
             * to be produced per pixel. This replaces the per-pixel arm below
             * for that one command kind; the two coverage arms of
             * g_ksn_row_coverage keep serving every other child. */
            if(g_ksn_tile_smooth&&d->kind==KSN_GRADIENT&&!d->data.gradient.radius){
                {KSN_PROF_BEGIN();
                smooth_block(tile,command,lo,hi-lo,x0+lo,py,has_dither,child_dither,dither_pixels);
                KSN_PROF_END(blend);}
#ifdef KSN_TILE_COUNT
                ksn_tile_smooth_blocks++;ksn_tile_smooth_pixels+=(uint32_t)(hi-lo);
#endif
                continue;
            }
            {KSN_PROF_BEGIN();
            if(g_ksn_row_coverage){
                ksn_x_run runs[KSN_ROW_RUNS];
                unsigned run_count=coverage_runs(command,py,x0,x0+count,runs);
                /* This row's colours, built once for the window the runs cover
                 * (the window the solver just named), then read per pixel. The
                 * predicate arm below has no such window -- solving one is what
                 * g_ksn_row_coverage does -- so it asks `sample` as always. */
                const ksn_row_table *row=run_count?
                    row_table_for(command,py,runs[0].x0,runs[run_count-1].x1):NULL;
                for(unsigned run=0;run<run_count;run++){
                    int from=runs[run].x0,to=runs[run].x1;
                    if(row)for(int px=from;px<to;px++)
                        group_pixel(tile,command,px-x0,has_dither,child_dither,dither_pixels,
                                    scratch->text[px-x0],row_table_value(row,px));
                    else for(int px=from;px<to;px++)
                        group_pixel(tile,command,px-x0,has_dither,child_dither,dither_pixels,
                                    scratch->text[px-x0],sample(command,px,py));
                }
            }else for(int x=lo;x<hi;x++)if(covers(command,x0+x,py))
                group_pixel(tile,command,x,has_dither,child_dither,dither_pixels,
                            scratch->text[x],sample(command,x0+x,py));
            KSN_PROF_END(blend);}
        }
#ifdef KSN_TILE_COUNT
        ksn_tile_visited+=(uint32_t)count;
        for(int x=0;x<count;x++)if(tile[x].a)ksn_tile_covered++;
#endif
        {KSN_PROF_BEGIN();
        tile_row_over(pixels,tile,count,opacity,has_dither,dither_pixels,x0,y,py);
        KSN_PROF_END(blend);}
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
    /* One frame's worth of decoded commands; a retried frame starts over. */
    memset(decoded.valid,0,sizeof(decoded.valid));decoded.text_used=0;
    ksn_frame frame;ksn_result result=ksn_core_prepare_frame(core,&frame);
    if(result!=KSN_OK)return result;
    uint32_t mask;result=ksn_core_damage(core,frame.ticket,&mask);
    if(result!=KSN_OK){ksn_core_defer_repair(core,frame.ticket);return result;}
    if(!mask)return ksn_core_presented(core,frame.ticket);
    const ksn_frame_view *command;
    for(unsigned layer=0;layer<2;layer++)for(unsigned i=0;i<frame.next[layer].commands;i++){
        {KSN_PROF_BEGIN();
        result=frame_command(core,frame.ticket,(ksn_layer)layer,(uint16_t)i,&command);
        KSN_PROF_END(read);}
        if(result!=KSN_OK){ksn_core_defer_repair(core,frame.ticket);return result;}
        if(command->draw.kind<KSN_RECT||command->draw.kind>KSN_IMAGE){
            ksn_core_defer_repair(core,frame.ticket);return KSN_UNSUPPORTED;
        }
        if(command->draw.kind==KSN_TEXT){
            {KSN_PROF_BEGIN();
            result=display->text&&display->text->span?
                display->text->span(display->text->ctx,&command->draw,command->reveal,0,0,0,NULL):KSN_UNSUPPORTED;
            KSN_PROF_END(span);}
            if(result!=KSN_OK){ksn_core_defer_repair(core,frame.ticket);return result;}
        }
        if(command->draw.kind==KSN_IMAGE){
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
        {KSN_PROF_BEGIN();
        fill565(pixels,(unsigned)(240*rows),rgb565(frame.next_background));
        KSN_PROF_END(fill);}
        for(unsigned layer=0;layer<2;layer++)for(unsigned i=0;i<frame.next[layer].commands;i++){
            {KSN_PROF_BEGIN();
            result=frame_command(core,frame.ticket,(ksn_layer)layer,(uint16_t)i,&command);
            KSN_PROF_END(read);}
            if(result!=KSN_OK){ksn_core_failed(core,frame.ticket);return result;}
            if(command->group_begin){
                unsigned first=i;uint8_t opacity=command->group_opacity;
                while(!command->group_end){
                    if(++i>=frame.next[layer].commands){ksn_core_failed(core,frame.ticket);return KSN_INVALID;}
                    {KSN_PROF_BEGIN();
                    result=frame_command(core,frame.ticket,(ksn_layer)layer,(uint16_t)i,&command);
                    KSN_PROF_END(read);}
                    if(result!=KSN_OK){ksn_core_failed(core,frame.ticket);return result;}
                }
                {KSN_PROF_BEGIN();
                result=render_group(core,display->text,&scratch,frame.ticket,(ksn_layer)layer,first,i,opacity,y,rows,pixels);
                KSN_PROF_END(tile);}
                if(result!=KSN_OK){ksn_core_failed(core,frame.ticket);return result;}
                continue;
            }
            const ksn_draw *d=&command->draw;
            if(!command->visible||!d->opacity)continue;
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
                    {KSN_PROF_BEGIN();
                    result=display->text->span(display->text->ctx,d,command->reveal,x,py,count,scratch.text);
                    KSN_PROF_END(span);}
                    if(result!=KSN_OK){ksn_core_failed(core,frame.ticket);return result;}
                    {KSN_PROF_BEGIN();
                    for(unsigned i=0;i<count;i++)if(scratch.text[i]){
                        unsigned index=(unsigned)((py-y)*240+x)+i;
                        ksn_rgba color=(d->data.text.color&0xffffff00u)|mul8(d->data.text.color&255,scratch.text[i]);
                        pixels[index]=blend(pixels[index],color,d->opacity,false,x+(int)i,py);
                    }
                    KSN_PROF_END(blend);}
                }
                continue;
            }
            if(d->kind==KSN_IMAGE){
                for(int py=y0;py<y1;py++)for(int x=x0;x<x1;){
                    unsigned count=(unsigned)(x1-x);if(count>16)count=16;
                    result=image_read(core,frame.ticket,(ksn_layer)layer,i,d,x,py,&count,&scratch);
                    if(result!=KSN_OK){ksn_core_failed(core,frame.ticket);return result;}
                    for(unsigned j=0;j<count;j++){
                        unsigned source=image_sample_index(d,&scratch,x,j),index=(unsigned)((py-y)*240+x)+j;
                        pixels[index]=blend(pixels[index],image_sample_color(d,&scratch,source),
                                            d->opacity,false,x+(int)j,py);
                    }
                    x+=(int)count;
                }
                continue;
            }
            if(d->kind==KSN_RECT&&d->opacity==255&&(d->data.shape.color&255)==255){
                uint16_t color=rgb565(d->data.shape.color);
                for(int py=y0;py<y1;py++){
                    {KSN_PROF_BEGIN();
                    fill565(pixels+(py-y)*240+x0,(unsigned)(x1-x0),color);
                    KSN_PROF_END(fill);}
                }
                continue;
            }
            {KSN_PROF_BEGIN();
            for(int py=y0;py<y1;py++){
                if(g_ksn_row_coverage){
                    ksn_x_run runs[KSN_ROW_RUNS];
                    unsigned run_count=coverage_runs(command,py,x0,x1,runs);
                    /* One row's colours over the window the runs cover, then a
                     * load per pixel; the else side is the path this file has
                     * always run, kept for the switch off and for any row the
                     * builder declines. */
                    const ksn_row_table *row=run_count?
                        row_table_for(command,py,runs[0].x0,runs[run_count-1].x1):NULL;
                    for(unsigned run=0;run<run_count;run++){
                        int from=runs[run].x0,to=runs[run].x1;
                        if(row)for(int x=from;x<to;x++){
                            unsigned index=(unsigned)((py-y)*240+x);
                            pixels[index]=blend(pixels[index],row_table_value(row,x),d->opacity,
                                                d->kind==KSN_GRADIENT&&d->data.gradient.dither,x,py);
                        }else for(int x=from;x<to;x++){
                            unsigned index=(unsigned)((py-y)*240+x);
                            pixels[index]=blend(pixels[index],sample(command,x,py),d->opacity,
                                                d->kind==KSN_GRADIENT&&d->data.gradient.dither,x,py);
                        }
                    }
                }else for(int x=x0;x<x1;x++)if(covers(command,x,py)){
                    unsigned index=(unsigned)((py-y)*240+x);
                    pixels[index]=blend(pixels[index],sample(command,x,py),d->opacity,
                                        d->kind==KSN_GRADIENT&&d->data.gradient.dither,x,py);
                }
            }
            KSN_PROF_END(blend);}
        }
        result=display->present(display->ctx,(uint16_t)y,(uint16_t)rows,pixels);
        if(result!=KSN_OK){ksn_core_failed(core,frame.ticket);return result;}
        stats->bands|=1u<<band;stats->transferred_bytes+=(uint32_t)rows*240u*2u;
    }
    return ksn_core_presented(core,frame.ticket);
}
