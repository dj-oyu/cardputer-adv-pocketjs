#include "ksn_render.h"
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
static const uint8_t bayer4[4][4]={{0,8,2,10},{12,4,14,6},{3,11,1,9},{15,7,13,5}};
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
static unsigned premultiply_over(ksn_premultiplied_rgba8 *dst,ksn_rgba color,uint8_t opacity){
    unsigned a=mul8(color&255,opacity),inverse=255-a;
    dst->r=(uint8_t)clamp8(mul8(color>>24,a)+mul8(dst->r,inverse));
    dst->g=(uint8_t)clamp8(mul8((color>>16)&255,a)+mul8(dst->g,inverse));
    dst->b=(uint8_t)clamp8(mul8((color>>8)&255,a)+mul8(dst->b,inverse));
    dst->a=(uint8_t)clamp8(a+mul8(dst->a,inverse));
    return a;
}
/* One covered group pixel: premultiplied RGBA into the tile, then the text
 * coverage word and the dither provenance bits. Both coverage arms call this,
 * so the pixels a row's runs name go through the same code the pixel-at-a-time
 * predicate path runs. `x` is the tile-local column. */
static void group_pixel(ksn_premultiplied_rgba8 *tile,const uint8_t *coverage,int x,
                        const ksn_frame_view *command,int px,int py,bool has_dither,
                        bool child_dither,uint32_t *dither_pixels){
    ksn_rgba color=sample(command,px,py);
    if(command->draw.kind==KSN_TEXT)color=(color&0xffffff00u)|mul8(color&255,coverage[x]);
    unsigned alpha=premultiply_over(&tile[x],color,command->draw.opacity);
    if(has_dither&&alpha){
        uint32_t bit=1u<<((unsigned)x&31u);
        if(child_dither)dither_pixels[(unsigned)x>>5]|=bit;
        else if(alpha==255)dither_pixels[(unsigned)x>>5]&=~bit;
    }
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
/* One row of a folded opaque group. A*src is the topmost covering child's own
 * channels and B and C are 0, so applying the group's map is one store; children
 * ascend and a later write wins, which is what "the tile's last writer" was.
 * `dither` is the topmost covering child's own provenance, and only when some
 * child of the group is a dithered gradient at all -- with has_dither false the
 * old chain's provenance bit was never set either. */
static ksn_result group_opaque_row(ksn_core *core,ksn_tx ticket,ksn_layer layer,unsigned first,
                                   unsigned end,int py,int band_y,bool has_dither,uint16_t *pixels){
    const ksn_frame_view *command;
    for(unsigned i=first;i<=end;i++){
        ksn_result result=frame_command(core,ticket,layer,(uint16_t)i,&command);
        if(result!=KSN_OK)return result;
        const ksn_draw *d=&command->draw;
        if(!command->visible||!d->opacity||py<d->bounds.y0||py>=d->bounds.y1||
           py<d->clip.y0||py>=d->clip.y1)continue;
        int left=d->bounds.x0>d->clip.x0?d->bounds.x0:d->clip.x0;
        int right=d->bounds.x1<d->clip.x1?d->bounds.x1:d->clip.x1;
        if(left<0)left=0;
        if(right>240)right=240;
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
static ksn_result render_group(ksn_core *core,const ksn_text_port *text,ksn_tx ticket,ksn_layer layer,unsigned first,unsigned end,
                               uint8_t opacity,int y,int rows,uint16_t *pixels){
    if(!opacity)return KSN_OK;
    ksn_premultiplied_rgba8 tile[64]; /* 256 bytes; no full component surface. */
    uint8_t coverage[64];
    const ksn_frame_view *command;
    bool has_dither=false;
    /* Step 1's precondition, collected where the children are already in hand:
     * one more call per child costs no read, and a group that fails it keeps the
     * pre-3c chain below bit for bit. */
    bool opaque_chain=true;
    int left=240,right=0,top=y+rows,bottom=y;
    for(unsigned i=first;i<=end;i++){
        ksn_result result;
        {KSN_PROF_BEGIN();
        result=frame_command(core,ticket,layer,(uint16_t)i,&command);
        KSN_PROF_END(read);}
        if(result!=KSN_OK)return result;
        const ksn_draw *d=&command->draw;
        if(!command->visible||!d->opacity)continue;
        if(!child_opaque(command))opaque_chain=false;
        int x0=d->bounds.x0>d->clip.x0?d->bounds.x0:d->clip.x0;
        int x1=d->bounds.x1<d->clip.x1?d->bounds.x1:d->clip.x1;
        int y0=d->bounds.y0>d->clip.y0?d->bounds.y0:d->clip.y0;
        int y1=d->bounds.y1<d->clip.y1?d->bounds.y1:d->clip.y1;
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
    /* Candidate 3c, step 1: opacity 255 over a chain of opaque children has no
     * floor to lose, so the tile, the per-child premultiply_over and the
     * per-pixel group_over are all dropped for one store per covered pixel. The
     * map is the identity on the source with the destination dropped (A=255,
     * B=C=0), which is why no pixel moves; the pixel set is the same solver the
     * switch-off arm below uses, so only the arithmetic between them differs. */
    if(g_ksn_group_affine&&opacity==255&&opaque_chain){
        {KSN_PROF_BEGIN();
        for(int py=top;py<bottom;py++){
            ksn_result result=group_opaque_row(core,ticket,layer,first,end,py,y,has_dither,pixels);
            if(result!=KSN_OK)return result;
        }
        KSN_PROF_END(blend);}
        return KSN_OK;
    }
    for(int py=top;py<bottom;py++)for(int x0=left;x0<right;x0+=64){
        int count=right-x0;if(count>64)count=64;
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
            if(!command->visible||!d->opacity||py<d->bounds.y0||py>=d->bounds.y1||
               py<d->clip.y0||py>=d->clip.y1||x0>=d->bounds.x1||x0>=d->clip.x1||
               x0+count<=d->bounds.x0||x0+count<=d->clip.x0)continue;
            if(command->draw.kind==KSN_TEXT){
                {KSN_PROF_BEGIN();
                result=text->span(text->ctx,&command->draw,command->reveal,x0,py,(unsigned)count,coverage);
                KSN_PROF_END(span);}
                if(result!=KSN_OK)return result;
            }
            {KSN_PROF_BEGIN();
            if(g_ksn_row_coverage){
                ksn_x_run runs[KSN_ROW_RUNS];
                unsigned run_count=coverage_runs(command,py,x0,x0+count,runs);
                for(unsigned run=0;run<run_count;run++)
                    for(int px=runs[run].x0;px<runs[run].x1;px++)
                        group_pixel(tile,coverage,px-x0,command,px,py,has_dither,child_dither,
                                    dither_pixels);
            }else for(int x=0;x<count;x++)if(covers(command,x0+x,py))
                group_pixel(tile,coverage,x,command,x0+x,py,has_dither,child_dither,dither_pixels);
            KSN_PROF_END(blend);}
        }
        {KSN_PROF_BEGIN();
        for(int x=0;x<count;x++){
            unsigned index=(unsigned)((py-y)*240+x0+x);
            bool dither=has_dither&&(dither_pixels[(unsigned)x>>5]&(1u<<((unsigned)x&31u)))!=0;
            pixels[index]=group_over(pixels[index],tile[x],opacity,dither,x0+x,py);
        }
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
        if(command->draw.kind<KSN_RECT||command->draw.kind>KSN_TEXT){
            ksn_core_defer_repair(core,frame.ticket);return KSN_UNSUPPORTED;
        }
        if(command->draw.kind==KSN_TEXT){
            {KSN_PROF_BEGIN();
            result=display->text&&display->text->span?
                display->text->span(display->text->ctx,&command->draw,command->reveal,0,0,0,NULL):KSN_UNSUPPORTED;
            KSN_PROF_END(span);}
            if(result!=KSN_OK){ksn_core_defer_repair(core,frame.ticket);return result;}
        }
    }
    uint16_t *pixels=display->strip(display->ctx);
    if(!pixels){ksn_core_defer_repair(core,frame.ticket);return KSN_OOM;}
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
                result=render_group(core,display->text,frame.ticket,(ksn_layer)layer,first,i,opacity,y,rows,pixels);
                KSN_PROF_END(tile);}
                if(result!=KSN_OK){ksn_core_failed(core,frame.ticket);return result;}
                continue;
            }
            const ksn_draw *d=&command->draw;
            if(!command->visible||!d->opacity)continue;
            int x0=d->bounds.x0,x1=d->bounds.x1,y0=d->bounds.y0,y1=d->bounds.y1;
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
                uint8_t coverage[64];
                for(int py=y0;py<y1;py++)for(int x=x0;x<x1;x+=64){
                    unsigned count=(unsigned)(x1-x);if(count>64)count=64;
                    {KSN_PROF_BEGIN();
                    result=display->text->span(display->text->ctx,d,command->reveal,x,py,count,coverage);
                    KSN_PROF_END(span);}
                    if(result!=KSN_OK){ksn_core_failed(core,frame.ticket);return result;}
                    {KSN_PROF_BEGIN();
                    for(unsigned i=0;i<count;i++)if(coverage[i]){
                        unsigned index=(unsigned)((py-y)*240+x)+i;
                        ksn_rgba color=(d->data.text.color&0xffffff00u)|mul8(d->data.text.color&255,coverage[i]);
                        pixels[index]=blend(pixels[index],color,d->opacity,false,x+(int)i,py);
                    }
                    KSN_PROF_END(blend);}
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
                    for(unsigned run=0;run<run_count;run++)for(int x=runs[run].x0;x<runs[run].x1;x++){
                        unsigned index=(unsigned)((py-y)*240+x);
                        pixels[index]=blend(pixels[index],sample(command,x,py),d->opacity,
                                            d->kind==KSN_GRADIENT&&d->data.gradient.dither,x,py);
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
