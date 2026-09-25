#include "ksn_render.h"
#include "ksn_image_transform.h"
#include "ksn_p0_probe.h"
#include <string.h>
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#include "esp_cpu.h"
#ifdef KASANE_P4_DECODE_CYCLE_PROBE
#include "esp_log.h"
#endif
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
#ifdef KASANE_P4_DECODE_CYCLE_PROBE
static uint64_t decode_work_cycles,decode_empty_cycles;
static uint32_t decode_calls,decode_work_max;
void ksn_render_decode_cycle_reset(void){
    decode_work_cycles=decode_empty_cycles=0;
    decode_calls=decode_work_max=0;
}
void ksn_render_decode_cycle_report(void){
    uint64_t net=decode_work_cycles>decode_empty_cycles?
                 decode_work_cycles-decode_empty_cycles:0;
    ESP_LOGI("KSN_P4_DECODE",
             "calls=%lu work_cycles=%llu empty_cycles=%llu net_cycles=%llu net_mean_cycles=%llu max_work_cycles=%lu",
             (unsigned long)decode_calls,
             (unsigned long long)decode_work_cycles,
             (unsigned long long)decode_empty_cycles,
             (unsigned long long)net,
             (unsigned long long)(decode_calls?net/decode_calls:0),
             (unsigned long)decode_work_max);
}
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
 * band loop and the group composition read. Text points into the sealed bank;
 * no pointer escapes this rendering attempt or its display callbacks. The
 * banks cannot change while a frame is in flight: the renderer holds the
 * sealed ticket, a guest cannot start another builder before it is presented
 * or discarded, and the port
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
    ksn_frame_command read; /* The ABI storage of the reference read path. */
} decoded;

/* The type is the format tag: these channels are premultiplied, never straight. */
typedef struct { uint8_t r,g,b,a; } ksn_premultiplied_rgba8;
/* One shared span scratch across normal/group paths. Together with the group
 * tile (256), dither bits (8), and a provider's 128-byte row: 504 <= 512 bytes. */
typedef union {
    _Alignas(16) uint8_t text[64];
    struct { uint16_t rgb[32];uint8_t alpha[32];uint8_t stretch[16]; } image;
    struct { uint16_t rgb[16];uint8_t alpha[16];uint16_t block_rgb[16];uint8_t block_alpha[16]; } rotated;
} ksn_span_scratch;
_Static_assert(sizeof(ksn_span_scratch)+256+8+128<=512,"compositor/provider pixel scratch budget");
_Static_assert(_Alignof(ksn_span_scratch)>=16,"PIE text mask must be 8-byte aligned");
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
    int32_t rotation;
    uint32_t bounds_x,bounds_y,window;         /* affine inputs, packed pairwise */
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
static void anchor_put(int32_t rotation,uint32_t bx,uint32_t by,uint32_t window,int x,int y,
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
            uint32_t key_bx=(uint16_t)d->bounds.x0|((uint32_t)(uint16_t)d->bounds.x1<<16);
            uint32_t key_by=(uint16_t)d->bounds.y0|((uint32_t)(uint16_t)d->bounds.y1<<16);
            unsigned delta=(unsigned)(x-g_anchor_row.base_x);
            if(g_anchor_row.rotation==(int32_t)d->data.image.rotation&&
               g_anchor_row.bounds_x==key_bx&&g_anchor_row.bounds_y==key_by&&
               g_anchor_row.window==(sw|(sh<<16))&&g_anchor_row.row==y&&
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
                           (uint16_t)d->bounds.x0|((uint32_t)(uint16_t)d->bounds.x1<<16),
                           (uint16_t)d->bounds.y0|((uint32_t)(uint16_t)d->bounds.y1<<16),
                           sw|(sh<<16),x,y,step_u,step_v,sw,sh,um,vm,sx,remu,sy,remv);
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
/* Boundary 4a's arithmetic: the 255 -> 256 coarse scale.
 *
 * The scalar blend/pack path divides by 255 exactly, through the compiler's
 * magic reciprocal (objdump: l32r + muluh + srli 7) or, in the lane model, the
 * (257*(y+1))>>16 identity (docs/perf/kasane-blend-pie.md 2.3). The panel is
 * 240x135 RGB565, so a one-step error in an eight-bit channel is not something
 * the panel can show, and the user accepted that trade for this family; the 256
 * scale is also the shape the eight-lane kernel wants (a sixteen-bit product
 * and a shift, no reciprocal). This switch is that trade, with the exact form
 * kept beside it:
 *
 *   g_ksn_scale256 = 0  every /255 stays exact: the pre-change reference.
 *   g_ksn_scale256 = 1  every 255-weighted term becomes 256-weighted --
 *                       scale256(b) = b + (b>>7) = round(b*256/255), then a
 *                       plain >>8. A pair of complementary weights (b and
 *                       255-b) collapses into ONE rounded mix rather than two
 *                       rounded products, so it cannot move by more than the
 *                       single floor.
 *
 * What does not move, in either arm: every alpha. `mul8` on a colour alpha, the
 * group tile's alpha accumulation, the span coverage scaling and the tile
 * alpha group_over reads are all zero/non-zero tests somewhere downstream
 * (blend's `if(!a)`, group_over's `if(!alpha)`, group_pixel's dither
 * provenance). A 1 -> 0 flip there is not a one-step error, it is a pixel that
 * is composited or is not, so the coarse arm only touches the three RGB mixes,
 * whose result lands in pack565 and nowhere else.
 *
 * The default is the measured one, not the hoped-for one: the sweep in
 * tools/pie/models/scale256_model.c moves 140,838 of 3,888,000 panel pixels
 * (3.622%) over 120 whole frames, 13.4% of the direct path's pixels (17.9% when
 * dithered) and touches the group path's channels by up to 2 steps, so the
 * exact path stays the default and the coarse arm is switch-only. The numbers
 * and the objdump are in docs/perf/kasane-alpha256.md;
 * KSN_SCALE256_ARM=0|1 pins one arm at compile time so an instruction count is
 * the count of the arm that runs. */
int g_ksn_scale256=0;
#if defined(KSN_SCALE256_ARM) && KSN_SCALE256_ARM
#define KSN_SCALE256() (true)
#elif defined(KSN_SCALE256_ARM)
#define KSN_SCALE256() (false)
#else
#define KSN_SCALE256() (g_ksn_scale256!=0)
#endif
/* round(b*256/255), 0..256, in two instructions. Its complement is itself:
 * 256 - scale256(b) == scale256(255-b) for all 256 b (the model proves it, and
 * that identity is what makes the complementary pair below a single mix). */
static inline __attribute__((always_inline)) unsigned scale256(unsigned b){return b+(b>>7);}
/* The mix of two terms weighted b and 255-b, in the 256 domain. The result is
 * a weighted average of x and y, so it stays within max(x,y) and needs no
 * clamp8 -- the model checks that over the whole domain. */
static inline __attribute__((always_inline)) unsigned mix256(unsigned x,unsigned y,unsigned b){
    unsigned w=scale256(b);
    return (x*w+y*(256-w))>>8;
}
/* x*b/255 as one product, for the place where the two weights do not
 * complement (group_over: the tile has already had src.a applied). */
static inline __attribute__((always_inline)) unsigned mul8_256(unsigned x,unsigned b){return (x*scale256(b))>>8;}
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
    if(KSN_SCALE256()){
        /* The two weights are a and 255-a: they complement, so each channel is
         * one rounded mix. No clamp8: mix256 cannot leave 0..255. The alpha
         * below stays exact in both arms. */
        dst->r=(uint8_t)mix256(color>>24,dst->r,a);
        dst->g=(uint8_t)mix256((color>>16)&255u,dst->g,a);
        dst->b=(uint8_t)mix256((color>>8)&255u,dst->b,a);
    }else{
        dst->r=(uint8_t)clamp8(mul8(color>>24,a)+mul8(dst->r,inverse));
        dst->g=(uint8_t)clamp8(mul8((color>>16)&255,a)+mul8(dst->g,inverse));
        dst->b=(uint8_t)clamp8(mul8((color>>8)&255,a)+mul8(dst->b,inverse));
    }
    dst->a=(uint8_t)clamp8(a+mul8(dst->a,inverse));
    return a;
}
static unsigned quantize(unsigned value,unsigned maximum,unsigned bayer){
    if(KSN_SCALE256()){
        /* floor(v*M/255) as (scale256(v)*M)>>8, with the dither test folded
         * into the same domain: 32*rem > (2b+1)*255 is rem > 8*(2b+1) once the
         * remainder counts in 256ths. The model checks the fold and the
         * quotient over the whole (value,maximum,bayer) space. */
        unsigned product=scale256(value)*maximum;
        unsigned q=product>>8,remainder=product&255u;
        if(q<maximum&&remainder>8u*(2u*bayer+1u))q++;
        return q;
    }
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
#ifdef KSN_COUNT_VISIBLE
/* ---- Candidate 4e "visible threshold skip": the measurement -------------- */
static uint16_t blend(uint16_t dst,ksn_rgba src,uint8_t opacity,bool dither,int x,int y);
static uint16_t rgb565(ksn_rgba c);
/* A 5/6/5 panel quantizes every channel to 32/64/32 levels, so a blend whose
 * source and destination name the same level (or a neighbour) can be left
 * unwritten. This counts how often that is true, per command kind and per
 * dither arm, and what skipping would actually cost in the written word: the
 * criteria are evaluated against the SOURCE, `moved`/`worst` are the chain's
 * own word on the pixels a criterion would skip -- moved pixels and the worst
 * channel step in 5/6/5 levels. No arithmetic here changes a pixel: these are
 * reads of the word the chain was about to write. Only compiled in with
 * -DKSN_COUNT_VISIBLE, so the shipping render path keeps its instruction
 * stream (main/ui/kasane/ksn_render.c object is byte-identical without it).
 *
 * Three arms, all sound for a non-dithering command (the proof is in the
 * commit message and the test):
 *   0 `step`: the source's own levels are within one step of dst in every
 *     channel. The blend output lies between src and the destination's
 *     expanded 8-bit channels, and quantizing is monotone, so the written
 *     level is between the two levels and never further than one step.
 *   1 `bound`: (alpha * max channel |src8 - dst8|) <= 2*255. The blend output
 *     is within alpha/255 * that distance + 0.5 of dst8; any 8-bit distance
 *     <= 3 keeps the g level (step 4) and the r/b levels (step 8) within one
 *     step. This arm catches the anti-aliased text edge (tiny alpha, any
 *     source) that arm 0 misses and arm 0 catches the case arm 1 misses
 *     (opaque source, same level).
 *   2 both: arms 0 and 1 together, the criterion the skip would ship with.
 *   3 exact: arm 0 with zero steps. This is the only arm a dithering command
 *     may use: quantize() adds at most one level on top of the written level,
 *     so an arm that already allows one step reaches two with dither on.
 * Arms 0 and 1 are not nested, which is why the union has to be counted. */
#define KSN_VIS_TEXT 0
#define KSN_VIS_RECT 1
#define KSN_VIS_ROUND_RECT 2
#define KSN_VIS_STROKE 3
#define KSN_VIS_GRADIENT 4
#define KSN_VIS_GROUP 5
#define KSN_VIS_KINDS 6
#define KSN_VIS_ARMS 4 /* step, bound, both, exact */
typedef struct {
    uint64_t empty;       /* chain returned dst because alpha was 0 (already free) */
    uint64_t pixels;      /* blend points that reached the chain */
    uint64_t exact;       /* the source's own levels equal dst in all three channels */
    uint64_t chain_moved; /* every counted pixel: the chain's word differed from dst */
    uint64_t chain_worst; /* every counted pixel: worst channel step */
    uint64_t sel[KSN_VIS_ARMS];   /* pixels each arm would skip */
    uint64_t moved[KSN_VIS_ARMS]; /* of those: the chain's word differed from dst */
    uint64_t worst[KSN_VIS_ARMS]; /* of those: worst channel step the chain moved */
} ksn_visible_slot;
ksn_visible_slot g_ksn_visible[KSN_VIS_KINDS*2];
void ksn_visible_reset(void){memset(g_ksn_visible,0,sizeof(g_ksn_visible));}
static unsigned visible_kind(unsigned kind){
    switch(kind){
    case KSN_TEXT:return KSN_VIS_TEXT;
    case KSN_ROUND_RECT:return KSN_VIS_ROUND_RECT;
    case KSN_STROKE:return KSN_VIS_STROKE;
    case KSN_GRADIENT:return KSN_VIS_GRADIENT;
    default:return KSN_VIS_RECT;
    }
}
static unsigned step_of(uint16_t value,unsigned shift,unsigned mask){return (value>>shift)&mask;}
static unsigned abs_diff(unsigned a,unsigned b){return a>b?a-b:b-a;}
/* Worst per-channel step between two 5/6/5 words. */
static unsigned visible_steps(uint16_t a,uint16_t b){
    const unsigned shifts[3]={11,5,0},masks[3]={31,63,31};
    unsigned worst=0;
    for(unsigned n=0;n<3;n++){
        unsigned d=abs_diff(step_of(a,shifts[n],masks[n]),step_of(b,shifts[n],masks[n]));
        if(d>worst)worst=d;
    }
    return worst;
}
/* dst expanded back to 8 bits per channel, as blend() does before it mixes. */
static void visible_expand(uint16_t dst,unsigned *r,unsigned *g,unsigned *b){
    unsigned qr=step_of(dst,11,31),qg=step_of(dst,5,63),qb=step_of(dst,0,31);
    *r=(qr<<3)|(qr>>2);*g=(qg<<2)|(qg>>4);*b=(qb<<3)|(qb>>2);
}
static unsigned visible_span(ksn_rgba src,uint16_t dst){
    unsigned r,g,b;visible_expand(dst,&r,&g,&b);
    unsigned worst=abs_diff((src>>24)&255u,r);
    unsigned d=abs_diff((src>>16)&255u,g);if(d>worst)worst=d;
    d=abs_diff((src>>8)&255u,b);if(d>worst)worst=d;
    return worst;
}
static unsigned visible_alpha(ksn_rgba src,uint8_t opacity){
    return ((src&255u)*opacity+127u)/255u;
}
static void visible_count(unsigned slot,uint16_t dst,ksn_rgba src,uint8_t opacity,uint16_t out){
    ksn_visible_slot *s=&g_ksn_visible[slot];
    unsigned alpha=visible_alpha(src,opacity);
    uint16_t src565=rgb565(src);
    unsigned src_step=visible_steps(src565,dst),out_step=visible_steps(out,dst);
    bool arm[KSN_VIS_ARMS];
    arm[0]=src_step<=1u;
    arm[1]=alpha*visible_span(src,dst)<=2u*255u;
    arm[2]=arm[0]||arm[1];
    arm[3]=src_step==0u;
    s->pixels++;
    if(!src_step)s->exact++;
    if(out!=dst)s->chain_moved++;
    if(out_step>s->chain_worst)s->chain_worst=out_step;
    for(unsigned n=0;n<KSN_VIS_ARMS;n++)if(arm[n]){
        s->sel[n]++;
        if(out!=dst)s->moved[n]++;
        if(out_step>s->worst[n])s->worst[n]=out_step;
    }
}
static uint16_t visible_blend(unsigned slot,uint16_t dst,ksn_rgba src,uint8_t opacity,
                              bool dither,int x,int y){
    if(!visible_alpha(src,opacity)){g_ksn_visible[slot].empty++;return dst;}
    uint16_t out=blend(dst,src,opacity,dither,x,y);
    visible_count(slot,dst,src,opacity,out);
    return out;
}
#define KSN_BLEND(slot,dst,src,opacity,dither,x,y) \
    visible_blend((slot),(dst),(src),(opacity),(dither),(x),(y))
#define KSN_SLOT(kind,dither) (visible_kind(kind)*2u+((dither)?1u:0u))
#else
#define KSN_BLEND(slot,dst,src,opacity,dither,x,y) blend((dst),(src),(opacity),(dither),(x),(y))
#define KSN_SLOT(kind,dither) 0u
#endif
static uint16_t group_over(uint16_t dst,ksn_premultiplied_rgba8 src,uint8_t opacity,
                           bool dither,int x,int y){
    unsigned alpha=mul8(src.a,opacity);
    if(!alpha)return dst;
    unsigned inverse=255-alpha;
    unsigned r=dst>>11,g=(dst>>5)&63,b=dst&31;
    if(KSN_SCALE256()){
        /* Here the weights (opacity, 255-alpha) do not complement: the tile is
         * already premultiplied by src.a, so opacity and the tile's inverse are
         * independent. Each term takes the 256 scale on its own, the two floors
         * add, and the clamp stays. */
        unsigned wo=scale256(opacity),wi=scale256(inverse);
        r=clamp8(((src.r*wo)>>8)+((((r<<3)|(r>>2))*wi)>>8));
        g=clamp8(((src.g*wo)>>8)+((((g<<2)|(g>>4))*wi)>>8));
        b=clamp8(((src.b*wo)>>8)+((((b<<3)|(b>>2))*wi)>>8));
    }else{
        r=clamp8(mul8(src.r,opacity)+mul8((r<<3)|(r>>2),inverse));
        g=clamp8(mul8(src.g,opacity)+mul8((g<<2)|(g>>4),inverse));
        b=clamp8(mul8(src.b,opacity)+mul8((b<<3)|(b>>2),inverse));
    }
#ifdef KSN_COUNT_VISIBLE
    /* The group's own candidate word: the composite before dither, which is
     * what a cheap test inside this function could compare with dst. The
     * source for the counting criteria is the tile colour as the straight
     * 8-bit channels plus alpha, so the bound arm sees the same alpha this
     * function used. Pixels whose tile alpha is 0 returned above as `empty`. */
    {
        uint16_t out=pack565(r,g,b,dither,x,y);
        ksn_rgba straight=(ksn_rgba)((uint32_t)src.r<<24|(uint32_t)src.g<<16|
                                     (uint32_t)src.b<<8|src.a);
        visible_count(KSN_VIS_GROUP*2u+(dither?1u:0u),dst,straight,opacity,out);
        return out;
    }
#else
    return pack565(r,g,b,dither,x,y);
#endif
}
/* ------------------------------------------------------------------------- *
 * Boundary 4 of docs/perf/kasane-opt-survey.md: the direct blend chain as a
 * quantized-key lookup table.
 *
 * `blend` is called once per pixel and, per channel, is a pure function of
 * three things: the destination's own channel value (five or six bits, so 32 or
 * 64 of them), the command's sampled colour plus its effective alpha, and -- on
 * the dithered arm -- the 4x4 bayer threshold `pack565` quantizes against (16
 * values). A row of the table is one (colour, alpha, threshold) triple costed
 * once for all 32/64 destination values: 128 bytes for the three channels
 * (5+6+5 bits packed back). Two arms, both built here and both optional at
 * run time:
 *
 *  - solid: one sampled colour for the command, which is
 *    RECT/ROUND_RECT/STROKE and also a gradient whose `from` equals its `to`
 *    (`interpolate` returns `from` for every i). Rows = the 16 bayer thresholds
 *    when the command dithers, so the dither is inside the table and the pixel
 *    only does the bayer index the pack used to do itself; rows = 1 for the
 *    thin pack. This arm is exact by construction: each entry runs blend()'s own
 *    expression, and the harness compares it against blend() over the whole
 *    per-channel space and over 120 frames.
 *  - alpha: the text path, whose three source channels are constant per command
 *    but whose effective alpha mul8(mul8(colour alpha, coverage), opacity)
 *    varies with the coverage byte. That is where a 16-level parameter
 *    quantization lives: the row is a>>4, built with the bucket midpoint
 *    (level*16+8, at most 248), so the approximation is bounded by 8 in the
 *    effective alpha and the harness measures what it does to pixels. This is
 *    the only arm that can move a pixel.
 *
 * The rows are rebuilt only when the (colour, opacity, dither) key changes; the
 * band loop repeats the same commands once per 8-row strip, so a full-frame
 * command builds its rows once a frame. The tables are static: the render path
 * has no allocator, and this is the layer the 5,824-byte decode cache already
 * lives in. 16+16 rows x 128 B = 4,096 B of table plus 16 B of keys = 4,112 B of
 * .bss, the size the survey's budget check named (a 32/64-value x 16-level table
 * per channel is 512-1,024 B); `nm -S` prints both arrays at 0x800.
 *
 * Both switches are A/B arms in ONE binary, the way g_ksn_row_coverage and
 * g_ksn_decode_once are: the same kernel moves ~15% between builds from
 * instruction cache alignment alone (CLAUDE.md), which is larger than anything
 * this change can win, so it has to be flipped inside one build. g_ksn_blend_lut
 * defaults to 1 (the solid arm is measured pixel-exact, worst step 0) and
 * g_ksn_blend_lut_alpha to 0 (the 16-level alpha quantization moves pixels by
 * more than one step; docs/perf/kasane-lut.md has the counts). */
int g_ksn_blend_lut=1;
int g_ksn_blend_lut_alpha=0;

#define KSN_BLEND_LUT_ROWS 16
#define KSN_BLEND_LUT_ROW 128 /* [0,32) red 5 bits, [32,96) green 6, [96,128) blue 5 */
static uint8_t blend_lut_solid[KSN_BLEND_LUT_ROWS][KSN_BLEND_LUT_ROW];
static uint8_t blend_lut_alpha[KSN_BLEND_LUT_ROWS][KSN_BLEND_LUT_ROW];
/* The parameters the current rows were built for. Two keys, one per arm: the
 * band loop revisits the same commands, and a rebuild is 128 stores a row. */
static struct { ksn_rgba color; uint8_t opacity; bool dither,valid; } blend_lut_solid_key,
                                                                     blend_lut_alpha_key;
/* One row: blend()'s three channel expressions and pack565()'s narrowing, for
 * one effective alpha and one bayer threshold. `opacity` is the command's, so a
 * == mul8(colour alpha, opacity) exactly as blend computes it -- except in the
 * alpha arm, which passes opacity 255 and an already-scaled alpha so that this
 * expression reproduces the level's own a.
 * The 255 -> 256 coarse arm (g_ksn_scale256) is a switch on blend()'s
 * arithmetic, so a row built here has to follow the same switch: the source and
 * destination weights are complementary (a and 255-a), which is exactly the one
 * rounded mix mix256 computes, and the dither narrowing goes through quantize(),
 * which is switched as well. Without this the table would stay exact while the
 * chain it is compared against went coarse, and the two arms would disagree by
 * the coarse arm's own one-step error. */
static void blend_lut_build_row(uint8_t *row,ksn_rgba color,uint8_t opacity,
                                unsigned threshold,bool dither){
    unsigned a=mul8(color&255u,opacity),inverse=255u-a;
    unsigned sr=color>>24u,sg=(color>>16u)&255u,sb=(color>>8u)&255u;
    for(unsigned index=0;index<32;index++){
        unsigned d=(index<<3)|(index>>2),
                 v=KSN_SCALE256()?mix256(sr,d,a):(sr*a+d*inverse+127u)/255u;
        row[index]=(uint8_t)(dither?quantize(v,31,threshold):(v>>3));
    }
    for(unsigned index=0;index<64;index++){
        unsigned d=(index<<2)|(index>>4),
                 v=KSN_SCALE256()?mix256(sg,d,a):(sg*a+d*inverse+127u)/255u;
        row[32+index]=(uint8_t)(dither?quantize(v,63,threshold):(v>>2));
    }
    for(unsigned index=0;index<32;index++){
        unsigned d=(index<<3)|(index>>2),
                 v=KSN_SCALE256()?mix256(sb,d,a):(sb*a+d*inverse+127u)/255u;
        row[96+index]=(uint8_t)(dither?quantize(v,31,threshold):(v>>3));
    }
}
/* The per-pixel work of both arms: three reads where the chain was. `row` is
 * the 128-byte row for this pixel's parameter set. Same-binary A/B builds this
 * file without inlining so the harness can count how many pixels took it. */
static uint16_t blend_lut_pack(uint16_t dst,const uint8_t *row){
    return (uint16_t)((row[dst>>11]<<11)|(row[32+((dst>>5)&63u)]<<5)|row[96+(dst&31u)]);
}
/* False when the effective alpha is zero: blend() then returns the destination
 * for every pixel, so the caller keeps the reference chain and the pixels (and
 * the counts) stay exactly what they were. */
static bool blend_lut_solid_prime(ksn_rgba color,uint8_t opacity,bool dither){
    if(!mul8(color&255u,opacity))return false;
    if(blend_lut_solid_key.valid&&blend_lut_solid_key.color==color&&
       blend_lut_solid_key.opacity==opacity&&blend_lut_solid_key.dither==dither)return true;
    unsigned rows=dither?KSN_BLEND_LUT_ROWS:1;
    for(unsigned threshold=0;threshold<rows;threshold++)
        blend_lut_build_row(blend_lut_solid[threshold],color,opacity,threshold,dither);
    blend_lut_solid_key.color=color;blend_lut_solid_key.opacity=opacity;
    blend_lut_solid_key.dither=dither;blend_lut_solid_key.valid=true;
    return true;
}
static bool blend_lut_alpha_prime(ksn_rgba color,uint8_t opacity){
    if(!(color&255u))return false; /* every level's a is zero: nothing to draw */
    if(blend_lut_alpha_key.valid&&blend_lut_alpha_key.color==color&&
       blend_lut_alpha_key.opacity==opacity)return true;
    for(unsigned level=0;level<KSN_BLEND_LUT_ROWS;level++)
        blend_lut_build_row(blend_lut_alpha[level],(color&0xffffff00u)|(level*16u+8u),255,0,false);
    blend_lut_alpha_key.color=color;blend_lut_alpha_key.opacity=opacity;
    blend_lut_alpha_key.dither=false;blend_lut_alpha_key.valid=true;
    return true;
}
/* One decode, shared by both paths: the cache stores it for the frame, the
 * reference path stores it for the next read only. */
static void decode_view(ksn_frame_view *view,const ksn_frame_command *command){
#ifdef KASANE_P4_DECODE_CYCLE_PROBE
    uint32_t empty_begin=ksn_cycles(),empty_cycles=ksn_cycles()-empty_begin;
    uint32_t work_begin=ksn_cycles();
#endif
    view->draw=command->draw;
    view->visible=command->visible;
    view->group_begin=command->group_begin;
    view->group_end=command->group_end;
    view->group_opacity=command->group_opacity;
    view->reveal=command->reveal;
#ifdef KASANE_P4_DECODE_CYCLE_PROBE
    uint32_t work_cycles=ksn_cycles()-work_begin;
    decode_empty_cycles+=empty_cycles;
    decode_work_cycles+=work_cycles;
    decode_calls++;
    if(work_cycles>decode_work_max)decode_work_max=work_cycles;
#endif
    ksn_p0_probe_copy(KSN_P0_RENDER_DECODE_VIEW,
                      sizeof(view->draw)+sizeof(view->visible)+
                      sizeof(view->group_begin)+sizeof(view->group_end)+
                      sizeof(view->group_opacity)+sizeof(view->reveal));
}
static ksn_frame_view *view_slot(unsigned slot){
    /* The core bounds the index, the clamp only keeps the name total. */
    return &decoded.view[slot<KSN_COMMANDS?slot:0];
}
static void cache_view(unsigned slot,const ksn_frame_command *command){
    ksn_frame_view *view=view_slot(slot);
    decode_view(view,command);
    decoded.valid[slot>>5]|=1u<<(slot&31u);
}
/* previous=false only: the renderer never reads the displayed bank. */
static ksn_result frame_command(ksn_core *core,ksn_tx ticket,ksn_layer layer,uint16_t index,
                                const ksn_frame_view **out){
    unsigned slot=(layer==KSN_APP?0u:KSN_APP_COMMANDS)+index;
    if(g_ksn_decode_once&&slot<KSN_COMMANDS&&(decoded.valid[slot>>5]&(1u<<(slot&31u)))){
        *out=view_slot(slot);return KSN_OK;
    }
    ksn_result result=ksn_core_read_borrowed(core,ticket,false,layer,index,&decoded.read);
    if(result!=KSN_OK)return result;
    if(g_ksn_decode_once&&slot<KSN_COMMANDS){
        cache_view(slot,&decoded.read);
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
        // A channel may descend, so shifting its negative signed span is UB.
        step[c]=count>1?(int32_t)(((int64_t)span*65536)/(count-1)):0;
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
/* Candidate 3c of docs/perf/kasane-opt-survey.md, boundary 3 and 4
 * (docs/perf/kasane-group-affine.md). A group's chain is a chain of
 * premultiplied `over` writes into one tile followed by one `group_over`, and
 * every step of it floors: mul8 rounds to the nearest 8-bit value. When the
 * group's opacity is 255 and every one of its children is opaque, none of those
 * floors can move a pixel, and the whole chain collapses into a single affine
 * map per group,
 *
 *     out = A*src + B*dst + C
 *
 * with A, B and C combined once per group instead of once per child per pixel:
 * for an opaque chain A is 255 and B and C are 0 on every channel, so the map
 * is `out = src` -- a store of the topmost covering child's own value. That is
 * step 1 of this workstream and it is bit-exact (see `child_opaque` below); the
 * approximate extension to non-opaque children is step 2 and lives behind
 * g_ksn_group_affine==2, off by default.
 *
 * 1 (default) = fold the fully opaque chain, 0 = the pre-3c path: tile,
 * premultiply_over per child pixel, group_over per pixel. Both arms live in one
 * binary so a same-binary A/B compares pixels (test_group_affine.c does) and so
 * a switch that silently never matches cannot look like a speedup. Alignment
 * caveat: this change is smaller than the 15% instruction-cache drift CLAUDE.md
 * records, so the arms' *speed* is a device question; their pixels and their
 * instruction counts are host questions. */
int g_ksn_group_affine=1;

/* A child whose tile write is a plain overwrite at every pixel its geometry
 * covers: opacity 255 and a colour alpha of 255. Then a = mul8(255,255) = 255
 * and inverse = 0, so mul8(x,255) == x and mul8(y,0) == 0 on every channel and
 * the tile ends at this child's own values -- the previous tile contents cannot
 * survive, and `clamp8` never bites (every operand is at most 255). The same
 * `alpha == 255` is what clears the dither provenance bit in `group_pixel`, so
 * the folded arm can name the topmost covering child as the only source of a
 * pixel's dither too.
 *
 * TEXT is excluded: its alpha is mul8(colour alpha, coverage[x]), which is 255
 * only on the pixels whose coverage happens to be 255, and that set belongs to a
 * font rather than to this renderer. A GRADIENT interpolates per channel, and
 * with both ends at alpha 255 every interpolated alpha is exactly 255
 * (`interpolate`: (255*(last-i)+255*i+last/2)/last == 255 for length >= 1). */
static bool child_opaque(const ksn_frame_view *command){
    const ksn_draw *d=&command->draw;
    if(!command->visible||d->opacity!=255)return false;
    switch(d->kind){
    case KSN_RECT:case KSN_ROUND_RECT:case KSN_STROKE:
        return (d->data.shape.color&255u)==255u;
    case KSN_GRADIENT:
        return (d->data.gradient.from&255u)==255u&&(d->data.gradient.to&255u)==255u;
    default:return false;
    }
}
/* Candidate 3c, step 2 of docs/perf/kasane-group-affine.md: the same fold for a
 * chain that is NOT opaque, which is where the floors it drops can move a pixel.
 * The chain's tile write is `mul8(src, a) + mul8(tile, 255-a)`, one floor to the
 * nearest 8-bit value per child per channel. Keeping the same expression in 8.8
 * fixed point moves that floor eight bits down, and re-associating it
 *
 *     acc = acc + (((src<<8) - acc) * w + 128) >> 8,   w = round(a*256/255)
 *
 * (since acc*(256-w) == acc*256 - acc*w) is one multiply per channel per child
 * instead of two, with no clamp: the accumulator stays inside 0..255<<8 by
 * construction. `w` is exactly 256 at a = 255, so an opaque child still
 * overwrites the accumulator exactly (acc = src<<8, delta 0, nothing to round)
 * and the group stage reads back the very 8-bit value the old chain stored.
 * Step 2 is therefore bit-exact on every chain step 1 folds, and a pixel can only
 * move if a child with a < 255 covers it -- the harness measures that subset.
 *
 * The alpha accumulator and the group stage keep the old 8-bit formulas bit for
 * bit (clamp8(a + mul8(alpha, 255-a)) and alpha = mul8(tile.a, group opacity)),
 * so the dst weight, the transparency decision and the dither provenance rule are
 * unchanged: what moves is the premultiplied colour's floor and nothing else. */
#define KSN_ACC_ONE 256u
typedef struct { uint16_t r[32],g[32],b[32]; uint8_t a[32]; } ksn_fold_acc;
/* a in 8.8: 255 maps to 256 ("one"), which is what makes an opaque child an
 * exact overwrite of the accumulator. */
static unsigned fixed8(unsigned value){return (value*KSN_ACC_ONE+127u)/255u;}
/* One covered pixel of one child: the colour accumulator's single multiply-chain
 * step per channel, the pre-3c alpha step, and the same provenance rule
 * `group_pixel` applies (a child with a == 255 clears the bit, a dithered
 * gradient sets it). `i` is the pixel's index inside the chunk, which is also its
 * provenance bit. */
static void fold_pixel(ksn_fold_acc *acc,uint32_t *provenance,const ksn_frame_view *command,
                       const uint8_t *coverage,int i,int x,int y,bool has_dither,bool child_dither){
    const ksn_draw *d=&command->draw;
    ksn_rgba color=sample(command,x,y);
    unsigned source_alpha=color&255u;
    if(d->kind==KSN_TEXT)source_alpha=mul8(source_alpha,coverage[i]);
    unsigned a=mul8(source_alpha,d->opacity);
    unsigned w=fixed8(a);
    {
        int delta_r=((int)(color>>24)<<8)-(int)acc->r[i];
        int delta_g=((int)((color>>16)&255u)<<8)-(int)acc->g[i];
        int delta_b=((int)((color>>8)&255u)<<8)-(int)acc->b[i];
        acc->r[i]=(uint16_t)((int)acc->r[i]+(((delta_r*(int)w)+128)>>8));
        acc->g[i]=(uint16_t)((int)acc->g[i]+(((delta_g*(int)w)+128)>>8));
        acc->b[i]=(uint16_t)((int)acc->b[i]+(((delta_b*(int)w)+128)>>8));
    }
    acc->a[i]=(uint8_t)clamp8(a+mul8(acc->a[i],255u-a));
    if(has_dither&&a){
        uint32_t bit=1u<<(unsigned)i;
        if(child_dither)*provenance|=bit;
        else if(a==255)*provenance&=~bit;
    }
}
/* One 32-pixel chunk of one row of a folded group. 32 and not 64 because the
 * accumulators (3 x 32 x 2 + 32 = 224 B) live in the 256 bytes the isolated tile
 * already occupied, so the stack grows by nothing. The gate -- command list,
 * clip tests, the coverage solver, the span call for TEXT -- is the pre-3c path's
 * own gate, so only the arithmetic between them differs. A TEXT child's span is
 * taken for the chunk (32 pixels) where the pre-3c path took it for 64; the port
 * is per-pixel by contract, and the pixels are compared either way. */
static ksn_result group_folded_chunk(ksn_core *core,const ksn_text_port *text,ksn_tx ticket,
                                     ksn_layer layer,unsigned first,unsigned end,uint8_t opacity,
                                     int py,int x0,int band_y,int count,uint8_t *coverage,
                                     bool has_dither,ksn_fold_acc *acc,uint16_t *pixels){
    memset(acc->r,0,sizeof(acc->r));memset(acc->g,0,sizeof(acc->g));
    memset(acc->b,0,sizeof(acc->b));memset(acc->a,0,sizeof(acc->a));
    uint32_t provenance=0;
    const ksn_frame_view *command;
    for(unsigned i=first;i<=end;i++){
        ksn_result result=frame_command(core,ticket,layer,(uint16_t)i,&command);
        if(result!=KSN_OK)return result;
        const ksn_draw *d=&command->draw;
        if(!command->visible||!d->opacity||py<d->bounds.y0||py>=d->bounds.y1||
           py<d->clip.y0||py>=d->clip.y1||x0>=d->bounds.x1||x0>=d->clip.x1||
           x0+count<=d->bounds.x0||x0+count<=d->clip.x0)continue;
        if(d->kind==KSN_TEXT){
            {KSN_PROF_BEGIN();
            result=text->span(text->ctx,d,command->reveal,x0,py,(unsigned)count,coverage);
            KSN_PROF_END(span);}
            if(result!=KSN_OK)return result;
        }
        bool child_dither=d->kind==KSN_GRADIENT&&d->data.gradient.dither;
        int left=d->bounds.x0>d->clip.x0?d->bounds.x0:d->clip.x0;
        int right=d->bounds.x1<d->clip.x1?d->bounds.x1:d->clip.x1;
        if(left<x0)left=x0;
        if(right>x0+count)right=x0+count;
        {KSN_PROF_BEGIN();
        if(g_ksn_row_coverage){
            ksn_x_run runs[KSN_ROW_RUNS];
            unsigned run_count=coverage_runs(command,py,left,right,runs);
            for(unsigned run=0;run<run_count;run++)for(int x=runs[run].x0;x<runs[run].x1;x++)
                fold_pixel(acc,&provenance,command,coverage,x-x0,x,py,has_dither,child_dither);
        }else for(int x=left;x<right;x++)if(covers(command,x,py))
            fold_pixel(acc,&provenance,command,coverage,x-x0,x,py,has_dither,child_dither);
        KSN_PROF_END(blend);}
    }
    /* The group's own stage: the pre-3c `group_over`, on the folded accumulator.
     * alpha == 0 leaves the pixel alone, which is the old chain's early return. */
    {KSN_PROF_BEGIN();
    for(int i=0;i<count;i++){
        unsigned alpha=mul8(acc->a[i],opacity);
        if(!alpha)continue;
        unsigned inverse=255-alpha;
        uint16_t dst=pixels[(py-band_y)*240+x0+i];
        unsigned dr=dst>>11,dg=(dst>>5)&63,db=dst&31;
        dr=(dr<<3)|(dr>>2);dg=(dg<<2)|(dg>>4);db=(db<<3)|(db>>2);
        unsigned r=clamp8(mul8(acc->r[i]>>8,opacity)+mul8(dr,inverse));
        unsigned g=clamp8(mul8(acc->g[i]>>8,opacity)+mul8(dg,inverse));
        unsigned b=clamp8(mul8(acc->b[i]>>8,opacity)+mul8(db,inverse));
        bool dither=has_dither&&((provenance>>(unsigned)i)&1u)!=0;
        pixels[(py-band_y)*240+x0+i]=pack565(r,g,b,dither,x0+i,py);
    }
    KSN_PROF_END(blend);}
    return KSN_OK;
}
/* One row of a folded opaque group. A*src is the topmost covering child's own
 * channels and B and C are 0, so applying the group's map is one store; children
 * ascend and a later write wins, which is what "the tile's last writer" was.
 * `dither` is the topmost covering child's own provenance, and only when some
 * child of the group is a dithered gradient at all -- with has_dither false the
 * old chain's provenance bit was never set either. */
static ksn_result group_opaque_row(ksn_core *core,ksn_tx ticket,ksn_layer layer,unsigned first,
                                   unsigned end,int py,int band_y,int dx0,int dx1,
                                   bool has_dither,uint16_t *pixels){
    const ksn_frame_view *command;
    for(unsigned i=first;i<=end;i++){
        ksn_result result=frame_command(core,ticket,layer,(uint16_t)i,&command);
        if(result!=KSN_OK)return result;
        const ksn_draw *d=&command->draw;
        if(!command->visible||!d->opacity||py<d->bounds.y0||py>=d->bounds.y1||
           py<d->clip.y0||py>=d->clip.y1)continue;
        int left=d->bounds.x0>d->clip.x0?d->bounds.x0:d->clip.x0;
        int right=d->bounds.x1<d->clip.x1?d->bounds.x1:d->clip.x1;
        if(left<dx0)left=dx0;
        if(right>dx1)right=dx1;
        if(left>=right)continue;
        bool dither=has_dither&&d->kind==KSN_GRADIENT&&d->data.gradient.dither;
        {KSN_PROF_BEGIN();
        if(g_ksn_row_coverage){
            ksn_x_run runs[KSN_ROW_RUNS];
            unsigned run_count=coverage_runs(command,py,left,right,runs);
            for(unsigned run=0;run<run_count;run++)for(int x=runs[run].x0;x<runs[run].x1;x++){
                ksn_rgba color=sample(command,x,py);
                pixels[(py-band_y)*240+x]=pack565(color>>24,(color>>16)&255u,(color>>8)&255u,
                                                  dither,x,py);
            }
        }else for(int x=left;x<right;x++)if(covers(command,x,py)){
            ksn_rgba color=sample(command,x,py);
            pixels[(py-band_y)*240+x]=pack565(color>>24,(color>>16)&255u,(color>>8)&255u,
                                              dither,x,py);
        }
        KSN_PROF_END(blend);}
    }
    return KSN_OK;
}
/* `dx0`/`dx1` are the band's damaged columns: the group composites inside them
 * and nowhere else, because the strip outside them belongs to a band that has
 * already gone out. */
static ksn_result render_group(ksn_core *core,const ksn_text_port *text,ksn_span_scratch *scratch,ksn_tx ticket,ksn_layer layer,unsigned first,unsigned end,
                               uint8_t opacity,int y,int rows,int dx0,int dx1,uint16_t *pixels){
    if(!opacity)return KSN_OK;
    /* One 256-byte scratch for both arms, so the fold costs no stack: the
     * isolated premultiplied tile of the pre-3c chain, or step 2's 8.8
     * accumulators (3 x 32 x 2 + 32 = 224 B). Never both at once.
     * The tile arm is written against a plain array, so `tile` names the union's
     * tile member; the coverage word is scratch->text (2a retype), not a local. */
    union {
        ksn_premultiplied_rgba8 tile[64]; /* no full component surface. */
        ksn_fold_acc acc;
    } buf;
    ksn_premultiplied_rgba8 *const tile=buf.tile;
    const ksn_frame_view *command;
    bool has_dither=false;
    /* Step 1's precondition, collected where the children are already in hand:
     * one more call per child costs no read, and a group that fails it keeps the
     * pre-3c chain below bit for bit. */
    bool opaque_chain=true;
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
            /* Step 1's precondition is collected here, where the children are
             * already in hand: one more call per child costs no read, and a group
             * that fails it keeps the pre-3c chain bit for bit. The reach table
             * still gets an entry for every child (an invisible one reaches
             * nothing), so the block skip below keeps its own bookkeeping. */
            if(!child_opaque(command))opaque_chain=false;
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
    if(left<dx0)left=dx0;
    if(right>dx1)right=dx1;
    if(top<y)top=y;
    if(bottom>y+rows)bottom=y+rows;
    if(left>=right)return KSN_OK;
    /* Candidate 3c, step 2: every group, folded, with the colour chain's floor
     * moved from 8 bits to 8.8 and the alpha chain and group stage kept exact.
     * Bit-exact wherever every covering child is opaque (see group_folded_chunk),
     * so the harness checks both this arm's exactness there and the pixels it
     * moves elsewhere. 2 is not the default: this arm is the measured
     * approximation, and it is reached only when the switch asks for it. */
    if(g_ksn_group_affine>=2){
        {KSN_PROF_BEGIN();
        for(int py=top;py<bottom;py++)for(int x0=left;x0<right;x0+=32){
            int count=right-x0;if(count>32)count=32;
            ksn_result result=group_folded_chunk(core,text,ticket,layer,first,end,opacity,py,x0,y,
                                                 count,scratch->text,has_dither,&buf.acc,pixels);
            if(result!=KSN_OK)return result;
        }
        KSN_PROF_END(blend);}
        return KSN_OK;
    }
    /* Candidate 3c, step 1: opacity 255 over a chain of opaque children has no
     * floor to lose, so the tile, the per-child premultiply_over and the
     * per-pixel group_over are all dropped for one store per covered pixel. The
     * map is the identity on the source with the destination dropped (A=255,
     * B=C=0), which is why no pixel moves; the pixel set is the same solver the
     * switch-off arm below uses, so only the arithmetic between them differs.
     * It runs before the tile loops: where the fold's precondition holds, the
     * tile (and with it the block width and the reach table) is not consulted --
     * the pixel set and the arithmetic are the ones the switch-off arm proves. */
    if(g_ksn_group_affine&&opacity==255&&opaque_chain){
        {KSN_PROF_BEGIN();
        for(int py=top;py<bottom;py++){
            ksn_result result=group_opaque_row(core,ticket,layer,first,end,py,y,left,right,has_dither,pixels);
            if(result!=KSN_OK)return result;
        }
        KSN_PROF_END(blend);}
        return KSN_OK;
    }
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
        memset(buf.tile,0,sizeof(buf.tile));
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

/* ------------------------------------------------------------------------- *
 * Candidate 4a of docs/perf/kasane-opt-survey.md: the 565 blend/pack kernel
 * (docs/perf/kasane-blend-pie.md). One call blends eight destination pixels of a
 * run from one source colour and one opacity; `thresholds` is NULL for the thin
 * pack or the run's eight bayer values (the 4-cycle column phase the scalar
 * pack reads per pixel). PIE is coprocessor 3: owner task only, and the
 * destination must be 16-byte aligned.
 * Default on: the arms are pixel identical (the kernel is exact, not an
 * approximation), so which one runs was a device measurement. Measured on the
 * board (tools/host_kasane_opt_ab.py, one binary, 200 windows, 3 runs): with the
 * kernel arm off the app's render_ms is 3.41 vs 2.32 with it on, i.e. the arm is
 * worth 1.09 ms per frame on the 30-painted-frame window the harness samples
 * (row_cov, the coarser switch, is the other one above the noise floor at 2.78).
 * ------------------------------------------------------------------------- */
int g_ksn_blend_pie=1;
/* Binary font coverage may be consumed directly as eight 0/A PIE lanes.
 * Keep this candidate off until a same-image device A/B establishes a gain. */
int g_ksn_text_pie=0;
#ifdef KSN_TEXT_PIE_COUNT
uint32_t ksn_text_pie_blocks;
uint32_t ksn_text_pie_mixed_blocks;
#endif
void ksn_blend8_pie(uint16_t *pixels,int blocks,ksn_rgba src,uint8_t opacity,
                    const uint16_t *thresholds);
void ksn_blend8_mask_pie(uint16_t *pixels,const uint8_t *mask,int blocks,
                         ksn_rgba src,uint8_t opacity);
/* The kernel for one row's aligned window [first, first+8*blocks). The bayer
 * phase is built here because it depends on the absolute column: the scalar
 * pack reads bayer4[y&3][x&3], and the 4-cycle pattern is the same for every
 * 8-pixel block of the row, so the vector is built once per block start. */
static void blend_pie_row(uint16_t *row,int first,int count,int py,bool dither,
                          ksn_rgba color,uint8_t opacity){
    if(count<8)return;
    uint16_t thresholds[8];
    const uint16_t *thr=NULL;
    if(dither){
        for(unsigned i=0;i<8;i++)
            thresholds[i]=bayer4[(unsigned)py&3u][(unsigned)(first+(int)i)&3u];
        thr=thresholds;
    }
    ksn_blend8_pie(row+first,count>>3,color,opacity,thr);
}
static uint16_t rgb565(ksn_rgba c){return (uint16_t)((c>>27)<<11|((c>>18)&63)<<5|((c>>11)&31));}
static uint16_t blend(uint16_t dst,ksn_rgba src,uint8_t opacity,bool dither,int x,int y){
    unsigned a=((src&255)*opacity+127)/255;
    if(!a)return dst;
    if(a==255&&!dither)return rgb565(src);
    unsigned r=(dst>>11)&31,g=(dst>>5)&63,b=dst&31;
    r=(r<<3)|(r>>2);g=(g<<2)|(g>>4);b=(b<<3)|(b>>2);
    if(KSN_SCALE256()){
        /* a and 255-a complement, so this is one rounded mix per channel. */
        r=mix256((unsigned)(src>>24),r,a);
        g=mix256(((src>>16)&255u),g,a);
        b=mix256(((src>>8)&255u),b,a);
    }else{
        r=((src>>24)*a+r*(255-a)+127)/255;
        g=(((src>>16)&255)*a+g*(255-a)+127)/255;
        b=(((src>>8)&255)*a+b*(255-a)+127)/255;
    }
    return pack565(r,g,b,dither,x,y);
}
static ksn_result render_rects(ksn_core *core,const ksn_display_port *display,
                               ksn_backdrop_loader load_backdrop,bool occlusion_safe,
                               ksn_render_stats *stats){
    if(!core||!display||!stats||!display->strip||!display->present||
       display->width!=240||display->height!=135||display->strip_rows!=8)return KSN_INVALID;
    *stats=(ksn_render_stats){0};
    /* One frame's worth of decoded commands; a retried frame starts over. */
    memset(decoded.valid,0,sizeof(decoded.valid));
    uint32_t occluded=load_backdrop&&occlusion_safe?
        ksn_core_opaque_system_bands(core):0;
    ksn_frame frame;ksn_result result=ksn_core_prepare_frame(core,&frame);
    if(result!=KSN_OK)return result;
    ksn_damage damage;result=ksn_core_damage(core,frame.ticket,display->text,&damage);
    if(result!=KSN_OK){ksn_core_defer_repair(core,frame.ticket);return result;}
    damage.bands&=~occluded;
    if(!damage.bands)return ksn_core_presented(core,frame.ticket);
    /* Narrowing is a decision taken here, before any pixel is written: a band
     * composited over part of its width leaves the rest of the shared strip
     * holding the previous band's pixels, so a port that can only send whole
     * rows must be given whole rows to send.
     *
     * Two things decide it besides the port. The window is rounded OUT to 16
     * pixels, because the pixels it saves are worth less than the transfer path
     * it would cost: a packed row of a multiple of 16 pixels is a whole number
     * of 32-byte blocks at an aligned address, which is what keeps the byte
     * swap on board.c's PIE kernel and the background fill on fill_blocks. A
     * window that is not a multiple of 16 puts both on their scalar arms, and
     * that was measured to cost more than the columns saved (hello: 11,520 ->
     * 8,832 bytes but render 1.55 -> 1.65 ms and send 1.67 -> 1.88 ms).
     *
     * And a window that is nearly the whole width is not worth taking: it pays
     * three panel commands per band for the re-addressing that the full-width
     * path avoids entirely (board.c's next_row streaming), and saves too few
     * columns to cover them. KSN_NARROW_MAX is where that trade is drawn. It is
     * a threshold on the band, not the frame: a frame may narrow some bands and
     * send others whole. */
#define KSN_NARROW_MAX 192
    for(unsigned band=0;band<17;band++){
        if(!(damage.bands&(1u<<band)))continue;
        int x0=damage.x0[band]&~15,x1=(damage.x1[band]+15)&~15;
        if(x1>240)x1=240;
        if(!display->present_rect||x1-x0>KSN_NARROW_MAX){x0=0;x1=240;}
        damage.x0[band]=(int16_t)x0;damage.x1[band]=(int16_t)x1;
    }
    const ksn_frame_view *command;
    uint32_t next_system_opaque=0;
    for(unsigned layer=0;layer<2;layer++)for(unsigned i=0;i<frame.next[layer].commands;i++){
        {KSN_PROF_BEGIN();
        result=frame_command(core,frame.ticket,(ksn_layer)layer,(uint16_t)i,&command);
        KSN_PROF_END(read);}
        if(result!=KSN_OK){ksn_core_defer_repair(core,frame.ticket);return result;}
        if(command->draw.kind<KSN_RECT||command->draw.kind>KSN_IMAGE){
            ksn_core_defer_repair(core,frame.ticket);return KSN_UNSUPPORTED;
        }
        /* The first ungrouped SYSTEM command can establish a whole opaque
         * band before anything else in that layer reads the strip. This is
         * about the sealed *next* frame, unlike the committed-pixel mask above:
         * a newly posted notice still has to be transferred, but its backdrop
         * and APP pixels need not be computed underneath it. */
        if(load_backdrop&&occlusion_safe&&layer==KSN_SYSTEM&&i==0&&
           !command->group_begin&&!command->group_end&&command->visible&&
           command->draw.kind==KSN_RECT&&command->draw.opacity==255&&
           (command->draw.data.shape.color&255u)==255u&&
           command->draw.bounds.x0<=0&&command->draw.bounds.x1>=240&&
           command->draw.clip.x0<=0&&command->draw.clip.x1>=240){
            for(unsigned band=0;band<17;band++){
                int y0=(int)band*8,y1=y0+8;if(y1>135)y1=135;
                if(command->draw.bounds.y0<=y0&&command->draw.bounds.y1>=y1&&
                   command->draw.clip.y0<=y0&&command->draw.clip.y1>=y1)
                    next_system_opaque|=1u<<band;
            }
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
        if(!(damage.bands&(1u<<band)))continue;
        int y=(int)band*8,rows=band==16?7:8;
        const int dx0=damage.x0[band],dx1=damage.x1[band];
        if(load_backdrop&&!(next_system_opaque&(1u<<band))){
            result=load_backdrop(display->ctx,(uint16_t)y,(uint16_t)rows,pixels);
            if(result!=KSN_OK){ksn_core_failed(core,frame.ticket);return result;}
        }else if(!(next_system_opaque&(1u<<band))){KSN_PROF_BEGIN();
            /* Whole strip in one call when the band is whole, which is every band
             * of a REPLACE and of any repair; otherwise the damaged columns of each
             * row, because the columns between them are not ours to touch. */
            if(dx0==0&&dx1==240)fill565(pixels,(unsigned)(240*rows),rgb565(frame.next_background));
            else for(int r=0;r<rows;r++)
                fill565(pixels+r*240+dx0,(unsigned)(dx1-dx0),rgb565(frame.next_background));
            KSN_PROF_END(fill);}
        for(unsigned layer=0;layer<2;layer++)for(unsigned i=0;i<frame.next[layer].commands;i++){
            if(layer==KSN_APP&&(next_system_opaque&(1u<<band)))break;
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
                result=render_group(core,display->text,&scratch,frame.ticket,(ksn_layer)layer,first,i,opacity,y,rows,dx0,dx1,pixels);
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
            /* The band's damaged columns, not the panel's: everything below
             * indexes the strip, and the strip outside them still holds the
             * band that went out before this one. */
            if(x0<dx0)x0=dx0;
            if(x1>dx1)x1=dx1;
            if(y0<y)y0=y;
            if(y1>y+rows)y1=y+rows;
            if(x0>=x1||y0>=y1)continue;
            if(d->kind==KSN_TEXT){
                /* The alpha arm's rows are the 16 effective-alpha buckets.
                 * The coverage buffer is the frame scratch's (2a retype), the
                 * one the span below fills. */
                bool lut=g_ksn_blend_lut_alpha!=0&&blend_lut_alpha_prime(d->data.text.color,d->opacity);
                for(int py=y0;py<y1;py++)for(int x=x0;x<x1;x+=64){
                    unsigned count=(unsigned)(x1-x);if(count>64)count=64;
                    {KSN_PROF_BEGIN();
                    result=display->text->span(display->text->ctx,d,command->reveal,x,py,count,scratch.text);
                    KSN_PROF_END(span);}
                    if(result!=KSN_OK){ksn_core_failed(core,frame.ticket);return result;}
                    {KSN_PROF_BEGIN();
                    uint16_t *row=pixels+(py-y)*240;
                    bool pie_row=g_ksn_text_pie&&g_ksn_blend_pie&&
                                 display->text->binary_coverage&&!lut&&!KSN_SCALE256()&&
                                 (((uintptr_t)row&15u)==0u);
                    for(unsigned i=0;i<count;){
                        /* Both PIE memory ops clear low address bits: only a
                         * fully aligned destination/mask pair is handed over.
                         * The binary port promises 0/255 coverage, so mixed
                         * ink blocks need no scan or expanded-alpha copy. */
                        if(pie_row&&((x+(int)i)&7)==0&&
                           (((uintptr_t)(scratch.text+i)&7u)==0u)&&i+8u<=count){
                            /* A binary font has many empty blocks. Inspect the
                             * borrowed eight bytes in registers and submit only
                             * contiguous nonempty blocks to the PIE kernel. */
                            /* GCC otherwise emits an out-of-line memcpy even
                             * for eight aligned bytes on Xtensa. may_alias
                             * keeps this borrowed read defined while the
                             * alignment gate above permits two 32-bit loads. */
                            typedef uint64_t ksn_mask_word __attribute__((may_alias));
                            uint64_t ink=*(const ksn_mask_word *)(scratch.text+i);
                            if(!ink){i+=8u;continue;}
#ifdef KSN_TEXT_PIE_COUNT
                            if(ink!=UINT64_MAX)ksn_text_pie_mixed_blocks++;
#endif
                            unsigned end=i+8u;
                            while(end+8u<=count){
                                ink=*(const ksn_mask_word *)(scratch.text+end);
                                if(!ink)break;
#ifdef KSN_TEXT_PIE_COUNT
                                if(ink!=UINT64_MAX)ksn_text_pie_mixed_blocks++;
#endif
                                end+=8u;
                            }
                            unsigned blocks=(end-i)>>3;
                            ksn_blend8_mask_pie(row+x+i,scratch.text+i,(int)blocks,
                                                d->data.text.color,d->opacity);
#ifdef KSN_TEXT_PIE_COUNT
                            ksn_text_pie_blocks+=blocks;
#endif
                            i=end;
                            continue;
                        }
                        if(!scratch.text[i]){i++;continue;}
                        unsigned index=(unsigned)((py-y)*240+x)+i;
                        if(lut){
                            unsigned a=mul8(mul8(d->data.text.color&255,scratch.text[i]),d->opacity);
                            if(a)pixels[index]=blend_lut_pack(pixels[index],blend_lut_alpha[a>>4]);
                            i++;
                            continue;
                        }
                        ksn_rgba color=(d->data.text.color&0xffffff00u)|mul8(d->data.text.color&255,scratch.text[i]);
                        pixels[index]=KSN_BLEND(KSN_SLOT(KSN_TEXT,false),pixels[index],color,
                                                d->opacity,false,x+(int)i,py);
                        i++;
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
                        uint16_t rgb=d->data.image.rotation?scratch.rotated.rgb[source]:scratch.image.rgb[source];
                        uint8_t alpha=d->data.image.rotation?scratch.rotated.alpha[source]:scratch.image.alpha[source];
                        if(!alpha)continue;
                        /* RGB565 -> expanded RGB8 -> RGB565 is an identity for
                         * a fully opaque texel. Keep it in provider format:
                         * no redundant channel unpack, blend, or repack. */
                        if(alpha==255&&d->opacity==255)pixels[index]=rgb;
                        else pixels[index]=blend(pixels[index],image_color(rgb,alpha),
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
            bool dither=d->kind==KSN_GRADIENT&&d->data.gradient.dither;
            /* A gradient whose ends are equal samples the same colour for every
             * pixel, so its rows fit the table as well. */
            bool one_color=d->kind!=KSN_GRADIENT||d->data.gradient.from==d->data.gradient.to;
            const uint8_t (*lut)[KSN_BLEND_LUT_ROW]=(g_ksn_blend_lut&&one_color&&
                blend_lut_solid_prime(sample(command,x0,y0),d->opacity,dither))?blend_lut_solid:NULL;
            /* Candidate 4a: a constant-colour command's aligned 8-pixel blocks go
             * to the PIE kernel. It needs a 16-byte aligned destination, so the
             * band row's base is checked once here and the head and tail of each
             * run stay on the arms below. All three arms are exact, so which one
             * runs is a measurement, not a pixel decision. */
            bool pie=g_ksn_blend_pie&&one_color;
            for(int py=y0;py<y1;py++){
                const uint8_t *bayer_row=dither?bayer4[(unsigned)py&3u]:NULL;
                bool pie_row=pie&&(((uintptr_t)(pixels+(py-y)*240)&15u)==0u);
                if(g_ksn_row_coverage){
                    ksn_x_run runs[KSN_ROW_RUNS];
                    unsigned run_count=coverage_runs(command,py,x0,x1,runs);
                    /* One row's colours over the window the runs cover, then a
                     * load per pixel; the else side is the path this file has
                     * always run, kept for the switch off and for any row the
                     * builder declines. The lut arm is tried first and only
                     * engages for a constant-colour command -- the same colour
                     * the table would hold -- so the two arms agree. The chain
                     * calls go through KSN_BLEND, which is `blend` unless the
                     * counting build (-DKSN_COUNT_VISIBLE) is measuring the
                     * candidates for boundary 4e; the table read replaces the
                     * chain for its pixels, so those are not counted (the
                     * counting arm turns the table off to see the whole chain). */
                    const ksn_row_table *row=run_count?
                        row_table_for(command,py,runs[0].x0,runs[run_count-1].x1):NULL;
                    for(unsigned run=0;run<run_count;run++){
                        int from=runs[run].x0,to=runs[run].x1;
                        /* The kernel takes one colour for the whole block, so it
                         * is given only this run's 8-aligned window; the head,
                         * the tail, and every pixel when the switch is off, go
                         * through the arms -- whose value is the same one. */
                        int pie_first=from,pie_last=from;
                        if(pie_row){
                            pie_first=(from+7)&~7;
                            pie_last=to&~7;
                            if(pie_last-pie_first<8)pie_first=pie_last=from;
                        }
                        for(int x=from;x<to;){
                            if(x>=pie_first&&x<pie_last){
                                blend_pie_row(pixels+(py-y)*240,pie_first,pie_last-pie_first,py,
                                              dither,sample(command,pie_first,py),d->opacity);
                                x=pie_last;
                                continue;
                            }
                            unsigned index=(unsigned)((py-y)*240+x);
                            if(lut)pixels[index]=blend_lut_pack(pixels[index],
                                lut[bayer_row?(unsigned)bayer_row[(unsigned)x&3u]:0u]);
                            else pixels[index]=KSN_BLEND(KSN_SLOT(d->kind,dither),pixels[index],
                                                     row?row_table_value(row,x):sample(command,x,py),
                                                     d->opacity,dither,x,py);
                            x++;
                        }
                    }
                }else for(int x=x0;x<x1;x++)if(covers(command,x,py)){
                    unsigned index=(unsigned)((py-y)*240+x);
                    if(lut)pixels[index]=blend_lut_pack(pixels[index],
                        lut[bayer_row?(unsigned)bayer_row[(unsigned)x&3u]:0u]);
                    else pixels[index]=KSN_BLEND(KSN_SLOT(d->kind,dither),
                                             pixels[index],sample(command,x,py),d->opacity,
                                             dither,x,py);
                }
            }
            KSN_PROF_END(blend);}
        }
        result=(dx0==0&&dx1==240)?
            display->present(display->ctx,(uint16_t)y,(uint16_t)rows,pixels):
            display->present_rect(display->ctx,(uint16_t)dx0,(uint16_t)y,
                                  (uint16_t)(dx1-dx0),(uint16_t)rows,pixels);
        if(result!=KSN_OK){ksn_core_failed(core,frame.ticket);return result;}
        stats->bands|=1u<<band;
        stats->transferred_bytes+=(uint32_t)rows*(uint32_t)(dx1-dx0)*2u;
    }
    return ksn_core_presented(core,frame.ticket);
}
ksn_result ksn_render_rects(ksn_core *core,const ksn_display_port *display,ksn_render_stats *stats){
    return render_rects(core,display,NULL,false,stats);
}
ksn_result ksn_render_rects_backdrop(ksn_core *core,const ksn_display_port *display,
                                     ksn_backdrop_loader load,bool occlusion_safe,
                                     ksn_render_stats *stats){
    if(!load)return KSN_INVALID;
    return render_rects(core,display,load,occlusion_safe,stats);
}
