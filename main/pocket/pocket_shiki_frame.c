#include "pocket_shiki_frame.h"
#ifdef KSN_PROC_HOST_TEST
ksn_result pocket_kasane_proc_publish(void);
ksn_result pocket_kasane_proc_publish_at(unsigned surface,ksn_rect damage);
void pocket_kasane_invalidate(void);
JSValue pocket_kasane_proc_resource(JSContext *ctx);
JSValue pocket_kasane_proc_resource_at(JSContext *ctx,unsigned surface);
#else
#include "pocket_kasane.h"
#endif
#include "pocket_api.h"
#ifdef ESP_PLATFORM
#include "disp.h"
#else
/* Host tests (not all put main/hal on the path): nothing is ever in flight. */
static inline void disp_join(void) {}
static inline bool disp_busy(void) { return false; }
#endif
#include "ui/kasane/ksn_shiki_frame.h"
#include "ui/kasane/ksn_shiki.h"
#include "proc_band_hash.h"
#ifdef ESP_PLATFORM
#include "esp_cpu.h"
#endif
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#ifdef KASANE_PROC_JS_DIAGNOSTIC
#include "esp_cpu.h"
static uint32_t image_band_count,image_band_cycles,image_span_count,image_span_cycles;
void pocket_proc_image_prof_read(uint32_t *band_count,uint32_t *band_cycles,
                                 uint32_t *span_count,uint32_t *span_cycles){
    if(band_count)*band_count=image_band_count;
    if(band_cycles)*band_cycles=image_band_cycles;
    if(span_count)*span_count=image_span_count;
    if(span_cycles)*span_cycles=image_span_cycles;
    image_band_count=image_band_cycles=image_span_count=image_span_cycles=0;
}
#endif
#ifdef KASANE_MEGADEMO_TRACE
/* Separate from the JS diagnostic above, which is read per 30-frame window:
 * the trace needs these per host turn, and it times the JS-visible calls
 * (register includes reading the arrays, prepare is the analysis alone). */
#include "esp_cpu.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
static pocket_proc_trace trace;
/* Survives trace_take(): a commit and its view update can be turns apart. */
static bool trace_split;
void pocket_proc_trace_take(pocket_proc_trace *out){
    if(out)*out=trace;
    trace=(pocket_proc_trace){0};
}
void pocket_proc_trace_view(void){trace.view_n++;trace_split=false;}
void pocket_proc_trace_presented(void){if(trace_split)trace.split_n++;}
#endif
// Counts this file's allocations when CONFIG_POCKET_SHIKI_MEMSTAT is on
// (docs/kasane/shiki.md sec.4.5); nothing otherwise.
#define SHIKI_MS_FILE SHIKI_MS_PROC
#include "shiki_memstat.h"
// Frames, the engine's state and programs come from the Shiki heap when the
// session has one (kasane.shiki, docs/kasane/shiki-spec.md sec.9.2), from the
// system heap otherwise.
#include "shiki_heap.h"
/* Two-core P0 probe (docs/perf/two-core-pipeline.md sec.5): where the JS
 * turn first touches state the display would read. Device only. */
#if defined(ESP_PLATFORM)
#include "frame_bd.h"
#else
#define frame_bd_join_point(site) ((void)0)
#endif

/* Slots are reusable after unregister(); handles are not (next_handle only
 * grows), so a released handle can never alias a later program. */
#define PROC_HANDLES 32u
#define PROC_SURFACES 2u
/* A surface's frame buffer holds only the segments it declared
 * (createSurface({maxSegments})), allocated as the ksn_proc_frame header plus
 * that many entries. The full 1,024-entry frame is 10,246 B: DERBY WATCH's
 * heap once had a largest free block of 10,240 B, so a second surface at the
 * full size could not exist at all (docs/kasane/surface-segment-cap.md).
 * Nothing reads a frame past count (render_band, damage, image spans), and
 * nothing copies or clears a whole ksn_proc_frame here, which is what makes
 * the short allocation safe. A Shiki draw writes straight into the frame's
 * place, stopped at the room left (ksn_shiki_draw_index_cap), so no frame-
 * sized scratch exists (docs/kasane/shiki-jit.md sec.22). */
_Static_assert(offsetof(ksn_proc_frame,segments)+
               KSN_PROC_SEGMENTS*sizeof(ksn_proc_segment)==sizeof(ksn_proc_frame),
               "segments must be the frame's last member, with no tail padding");
static size_t frame_bytes(unsigned segments){
    return offsetof(ksn_proc_frame,segments)+(size_t)segments*sizeof(ksn_proc_segment);
}

/* plan is a ksn_shiki_plan (pocket.kasane.shiki, pocket_shiki_api.c), the
 * only kind since v2.0 removed the float VM. Handles never exceed
 * PROC_HANDLE_MAX. */
#define PROC_HANDLE_MAX ((1u<<30)-1u)
typedef struct {
    uint32_t handle;
    void *plan;
} proc_slot;
static proc_slot slots[PROC_HANDLES];
static uint32_t next_handle;
/* The Shiki engine's state for draw (pocket_proc_shiki_draw), taken with the
 * frames on the first Shiki draw. */
static ksn_shiki_vm *shiki_vm;
/* What a frame shows on each 8-row band of the image: a hash of the ordered
 * entries that touch the band (its pixels are a function of the backdrop
 * colour and that list alone, ksn_proc_render_band) and their columns. Two
 * frames whose band hashes match draw the band alike, so commit() damages
 * only the bands whose hash changed (pocket_proc_band_damage). */
#define PROC_BANDS ((KSN_PROC_H+7)/8)
typedef struct {
    uint32_t hash[PROC_BANDS];
    uint8_t x0[PROC_BANDS],x1[PROC_BANDS];
    /* The entries [first, end) hold every one that touches the band: a band
     * is drawn walking those alone (ksn_proc_render_band_range,
     * docs/platform/lcd-dma-driver.md 9.5). first > end: none touch it. */
    uint16_t first[PROC_BANDS],end[PROC_BANDS];
#ifdef POCKET_PROC_SIG_CHECK
    uint64_t strong[PROC_BANDS];   /* host only: the ground truth the hash is checked against */
#endif
} proc_band_sig;
/* One buffer a surface (docs/vm/qjs-observe.md sec.25). candidate and
 * committed are the same allocation, taken once at the first beginFrame
 * for maxSegments + spare entries and kept until reset: an allocation in
 * the middle of a session would cut the Shiki heap into pieces. The
 * committed frame's entries stay at its front, untouched until commit():
 * the panel may be redrawn from them at any time before then (a parked
 * frame(), another Kasane node changing, a thrown frame leaving them on
 * screen). The frame being built goes behind them (tail_used entries from
 * tail_base); commit() moves it to the front. So a frame fits when the
 * committed one and it fit maxSegments + spare (resource({spare}),
 * maxSegments by default: room for two full frames, as the two buffers
 * this replaced). */
typedef struct {
    ksn_proc_frame *candidate,*committed;   /* the same buffer */
    uint16_t tail_base;      /* the committed frame's count when the frame began, rounded up to even */
    uint16_t tail_used;      /* the new frame's entries behind it */
    uint16_t tail_raster;
    uint8_t tail_ext;
    bool committed_lost;     /* commit() wrote the new frame over the committed one */
    proc_band_sig candidate_sig,committed_sig;
    uint16_t candidate_color,committed_color;
    /* beginFrame's colour, for the frame being built: commit() makes it the
     * candidate's. candidate_color itself is the front frame's, which the
     * display may be drawing meanwhile (docs/perf/two-core-pipeline.md 5a). */
    uint16_t tail_color;
    bool pending,has_committed,repair_required,image_mode;
    bool sig_ready; /* candidate_sig describes the candidate (commit .. next beginFrame) */
    uint32_t handle;
    ksn_rect damage;
    uint16_t max_segments; /* 0 = KSN_PROC_SEGMENTS (no option) */
    uint16_t spare;        /* entries past max_segments, when spare_set */
    bool spare_set;        /* false: as many again as max_segments */
} proc_surface;
static proc_surface surfaces[PROC_SURFACES];
static uint32_t next_surface_handle;
static unsigned building_surface,pending_surface;
/* An image surface's raster cache: one 8-row band of pixels, taken with an
 * image surface's frames (3,840 B). It was the union with a full 1,024-entry
 * scratch frame (10,246 B) that every draw wrote into before the room check
 * and a copy into the candidate; draws now go straight to the candidate
 * (sec.22). */
#define PROC_IMAGE_BAND_ROWS 8u
static uint16_t *image_band_buf;
/* The 4-row pieces' note (ksn_proc_render_band_carry, 144 B from the Shiki
 * arena when it has room): which entries the first piece of a band saw reach
 * the second, so the second walks only those. Optional -- without it every
 * piece takes the whole range. Dropped whenever a frame's entries can change
 * (docs/platform/lcd-dma-driver.md 9.9). */
static ksn_proc_carry *half_carry;
static inline void carry_drop(void){if(half_carry)half_carry->valid=false;}
static bool building;
/* A source row can be requested repeatedly by scaled image draws. Cache the
 * whole eight-row raster band so adjacent rows share one segment traversal. */
static uint16_t image_band_y;
static const ksn_proc_frame *image_band_frame;
static uint16_t image_band_color;
static unsigned image_band_surface;
static bool image_band_valid;
/* JS array elements may be accessors or Proxy traps. Nested calls must not
 * mutate a plan slot or frame while an outer call is still reading fields. */
static bool js_call_active;

static JSValue failure(JSContext *ctx,const char *op,const char *code,const char *message){
    return pocket_api_throw(ctx,code,op,message,false,POCKET_OUTCOME_NOT_APPLIED);
}
static bool number(JSContext *ctx,JSValueConst value,double *out){
    return JS_IsNumber(value)&&JS_ToFloat64(ctx,out,value)==0&&isfinite(*out);
}
static bool integer(JSContext *ctx,JSValueConst value,uint32_t max,uint32_t *out){
    double n;
    if(!number(ctx,value,&n)||n<0||n>max||n!=floor(n))return false;
    *out=(uint32_t)n;return true;
}
static unsigned surface_segments(const proc_surface *surface){
    return surface->max_segments?surface->max_segments:KSN_PROC_SEGMENTS;
}
/* The buffer: the frame's own entries and the spare behind them. */
static unsigned surface_place(const proc_surface *surface){
    return surface_segments(surface)+(surface->spare_set?surface->spare:surface_segments(surface));
}
/* The surface's one frame buffer (candidate and committed), once. */
static bool buffers(proc_surface *surface){
    const size_t bytes=frame_bytes(surface_place(surface));
    if(!surface->candidate)surface->candidate=surface->committed=shiki_or_sys_calloc(1,bytes);
    if(surface->image_mode&&!image_band_buf) {
        /* The image band is the display task's while a present is in flight
         * (two-core-pipeline.md sec.1.2): made only with none in flight. */
        disp_join();
        image_band_buf=shiki_or_sys_calloc(KSN_PROC_W*PROC_IMAGE_BAND_ROWS,sizeof *image_band_buf);
    }
    return surface->candidate&&(!surface->image_mode||image_band_buf);
}
static bool any_pending(void){
    for(unsigned i=0;i<PROC_SURFACES;i++)if(surfaces[i].pending)return true;
    return false;
}
static bool any_repair(void){
    for(unsigned i=0;i<PROC_SURFACES;i++)if(surfaces[i].repair_required)return true;
    return false;
}
static bool surface_index(JSContext *ctx,JSValueConst value,unsigned *index){
    uint32_t handle;
    if(!integer(ctx,value,INT32_MAX,&handle))return false;
    if(handle==0){*index=0;return true;}
    for(unsigned i=1;i<PROC_SURFACES;i++)if(surfaces[i].handle==handle){
        *index=i;return true;
    }
    return false;
}
/* Handle 0 is never issued and is also what an empty slot holds, so a
 * lookup must require a plan, not just a matching number. */
static proc_slot *find_slot(uint32_t handle){
    if(!handle)return NULL;
    for(unsigned i=0;i<PROC_HANDLES;i++)
        if(slots[i].plan&&slots[i].handle==handle)return &slots[i];
    return NULL;
}
/* commit()'s two walks over every entry, at -O2 in a file kept at -Os: built
 * for size, these walks took 1.0 + 1.1 ms a frame in STRESS (~900 entries);
 * at -O2 and reading a frame without pattern lines field by field, 0.17 +
 * 0.65 ms (device, same-binary A/B, docs/platform/lcd-dma-driver.md 9.11).
 * A frame with pattern lines walks through ksn_proc_frame_next as before. */
#define PROC_HOT __attribute__((optimize("O2")))
/* frame_damage's bounding box with PIE (docs/vm/qjs-observe.md sec.19): ~18
 * cycles a segment in C (two frames of ~900 in STRESS, every commit). A
 * segment is 5 halfwords (x0 y0 x1 y1 color), so 8 of them are 5 vectors
 * whose lanes always hold the same fields: lane h of a group is field h % 5.
 * Five running minima, then five maxima (8 Q registers do not hold ten
 * accumulators and a load), then the lanes holding x and y are folded in C.
 * Exact: the same min/max as the scalar loop. Owner task only (PIE is
 * coprocessor 3), as every Shiki and Kasane kernel. */
#if defined(__xtensa__) && defined(CONFIG_IDF_TARGET_ESP32S3) && CONFIG_IDF_TARGET_ESP32S3
#define PROC_DAMAGE_PIE 1
#endif
#if defined(PROC_DAMAGE_PIE) || defined(PROC_DAMAGE_PIE_MODEL)
#ifndef PROC_DAMAGE_PIE_MODEL
static const int16_t damage_lo[8] __attribute__((aligned(16)))={INT16_MAX,INT16_MAX,INT16_MAX,INT16_MAX,
                                                                INT16_MAX,INT16_MAX,INT16_MAX,INT16_MAX};
static const int16_t damage_hi[8] __attribute__((aligned(16)))={INT16_MIN,INT16_MIN,INT16_MIN,INT16_MIN,
                                                                INT16_MIN,INT16_MIN,INT16_MIN,INT16_MIN};
#endif
/* seg: 16-byte aligned, groups > 0 groups of 8 segments; out: 16-byte
 * aligned, 40 halfwords. */
static void __attribute__((noinline)) damage_pie_min(const ksn_proc_segment *seg,unsigned groups,
                                                     int16_t *out){
#ifdef PROC_DAMAGE_PIE_MODEL
    for(unsigned h=0;h<40;h++)out[h]=INT16_MAX;
    for(unsigned g=0;g<groups;g++)for(unsigned h=0;h<40;h++){
        const int16_t v=((const int16_t *)seg)[g*40+h];if(v<out[h])out[h]=v;}
#else
    const int16_t *init=damage_lo;
    __asm__ volatile(
        "ee.vld.128.ip q0, %[init], 0\n"
        "ee.vld.128.ip q1, %[init], 0\n"
        "ee.vld.128.ip q2, %[init], 0\n"
        "ee.vld.128.ip q3, %[init], 0\n"
        "ee.vld.128.ip q4, %[init], 0\n"
        "loopgtz %[groups], 1f\n"
        "  ee.vld.128.ip q5, %[seg], 16\n"
        "  ee.vld.128.ip q6, %[seg], 16\n"
        "  ee.vmin.s16 q0, q0, q5\n"
        "  ee.vld.128.ip q5, %[seg], 16\n"
        "  ee.vmin.s16 q1, q1, q6\n"
        "  ee.vld.128.ip q6, %[seg], 16\n"
        "  ee.vmin.s16 q2, q2, q5\n"
        "  ee.vld.128.ip q5, %[seg], 16\n"
        "  ee.vmin.s16 q3, q3, q6\n"
        "  ee.vmin.s16 q4, q4, q5\n"
        "1:\n"
        "ee.vst.128.ip q0, %[out], 16\n"
        "ee.vst.128.ip q1, %[out], 16\n"
        "ee.vst.128.ip q2, %[out], 16\n"
        "ee.vst.128.ip q3, %[out], 16\n"
        "ee.vst.128.ip q4, %[out], 16\n"
        : [seg] "+&a"(seg), [out] "+&a"(out)
        : [groups] "a"(groups), [init] "a"(init)
        : "memory");
#endif
}
static void __attribute__((noinline)) damage_pie_max(const ksn_proc_segment *seg,unsigned groups,
                                                     int16_t *out){
#ifdef PROC_DAMAGE_PIE_MODEL
    for(unsigned h=0;h<40;h++)out[h]=INT16_MIN;
    for(unsigned g=0;g<groups;g++)for(unsigned h=0;h<40;h++){
        const int16_t v=((const int16_t *)seg)[g*40+h];if(v>out[h])out[h]=v;}
#else
    const int16_t *init=damage_hi;
    __asm__ volatile(
        "ee.vld.128.ip q0, %[init], 0\n"
        "ee.vld.128.ip q1, %[init], 0\n"
        "ee.vld.128.ip q2, %[init], 0\n"
        "ee.vld.128.ip q3, %[init], 0\n"
        "ee.vld.128.ip q4, %[init], 0\n"
        "loopgtz %[groups], 1f\n"
        "  ee.vld.128.ip q5, %[seg], 16\n"
        "  ee.vld.128.ip q6, %[seg], 16\n"
        "  ee.vmax.s16 q0, q0, q5\n"
        "  ee.vld.128.ip q5, %[seg], 16\n"
        "  ee.vmax.s16 q1, q1, q6\n"
        "  ee.vld.128.ip q6, %[seg], 16\n"
        "  ee.vmax.s16 q2, q2, q5\n"
        "  ee.vld.128.ip q5, %[seg], 16\n"
        "  ee.vmax.s16 q3, q3, q6\n"
        "  ee.vmax.s16 q4, q4, q5\n"
        "1:\n"
        "ee.vst.128.ip q0, %[out], 16\n"
        "ee.vst.128.ip q1, %[out], 16\n"
        "ee.vst.128.ip q2, %[out], 16\n"
        "ee.vst.128.ip q3, %[out], 16\n"
        "ee.vst.128.ip q4, %[out], 16\n"
        : [seg] "+&a"(seg), [out] "+&a"(out)
        : [groups] "a"(groups), [init] "a"(init)
        : "memory");
#endif
}
_Static_assert(sizeof(ksn_proc_segment)==10,"damage_pie: 8 segments are 5 vectors");
#endif
/* One plain segment into the box (min, min, max+1, max+1). */
static inline __attribute__((always_inline)) void damage_one(const ksn_proc_segment *seg,
                                                             int *x0,int *y0,int *x1,int *y1){
    const int sx0=seg->x0,sx1=seg->x1,sy0=seg->y0,sy1=seg->y1;
    const int ax=sx0<sx1?sx0:sx1,bx=sx0^sx1^ax,ay=sy0<sy1?sy0:sy1,by=sy0^sy1^ay;
    if(ax<*x0)*x0=ax;
    if(ay<*y0)*y0=ay;
    if(bx+1>*x1)*x1=bx+1;
    if(by+1>*y1)*y1=by+1;
}
/* The bounding box of n plain segments. */
PROC_HOT static void damage_plain(const ksn_proc_segment *seg,unsigned n,
                                  int *x0,int *y0,int *x1,int *y1){
#if defined(PROC_DAMAGE_PIE) || defined(PROC_DAMAGE_PIE_MODEL)
    /* PIE rounds the address down: peel to a 16-byte boundary (10-byte steps
     * from an even address reach one within 8), then whole groups of 8. */
    while(n&&((uintptr_t)seg&15u)){damage_one(seg++,x0,y0,x1,y1);n--;}
    const unsigned groups=n/8u;
    if(groups){
        int16_t lo[40] __attribute__((aligned(16))),hi[40] __attribute__((aligned(16)));
        damage_pie_min(seg,groups,lo);
        damage_pie_max(seg,groups,hi);
        for(unsigned h=0;h<40;h++){
            const unsigned field=h%5u;
            if(field==4)continue;                     /* colour */
            int *const a=field&1?y0:x0,*const b=field&1?y1:x1;   /* y0,y1 : x0,x1 */
            if(lo[h]<*a)*a=lo[h];
            if(hi[h]+1>*b)*b=hi[h]+1;
        }
        seg+=groups*8u;n-=groups*8u;
    }
#endif
    for(const ksn_proc_segment *const stop=seg+n;seg<stop;seg++)damage_one(seg,x0,y0,x1,y1);
}
/* The entries' box, unclamped, into *x0..*y1 (frame_damage). The running
 * box is kept in locals: through the pointers it was loaded and stored for
 * every entry (+7,600 instructions a STRESS commit on r5sim). */
PROC_HOT static void damage_add(const ksn_proc_frame *frame,int *bx0,int *by0,int *bx1,int *by1){
    int x0=*bx0,y0=*by0,x1=*bx1,y1=*by1;
    if(!frame->ext){
        damage_plain(frame->segments,frame->count,&x0,&y0,&x1,&y1);
    }else{
        unsigned i=0,cost;
        ksn_proc_segment geometry;
        bool pattern;
        /* Geometry only: a pattern line's parameters are not coordinates. */
        while(ksn_proc_frame_next(frame,&i,&geometry,&pattern,&cost)){
            const ksn_proc_segment *seg=&geometry;
            int ax=seg->x0<seg->x1?seg->x0:seg->x1;
            int bx=seg->x0>seg->x1?seg->x0:seg->x1;
            int ay=seg->y0<seg->y1?seg->y0:seg->y1;
            int by=seg->y0>seg->y1?seg->y0:seg->y1;
            if(ax<x0)x0=ax;
            if(ay<y0)y0=ay;
            if(bx+1>x1)x1=bx+1;
            if(by+1>y1)y1=by+1;
        }
    }
    *bx0=x0;*by0=y0;*bx1=x1;*by1=y1;
}
/* The committed frame's box, taken before commit() moves the new frame
 * over it (whole: no committed frame, or another colour). */
typedef struct { int x0,y0,x1,y1; bool whole; } proc_box;
static proc_box old_box(const proc_surface *surface){
    proc_box b={KSN_PROC_W,KSN_PROC_H,0,0,
                !surface->has_committed||surface->tail_color!=surface->committed_color};
    if(!b.whole)damage_add(surface->committed,&b.x0,&b.y0,&b.x1,&b.y1);
    return b;
}
/* The candidate's damage: the box of both frames' entries. */
PROC_HOT static ksn_rect frame_damage(const proc_surface *surface,proc_box b){
    if(b.whole)return (ksn_rect){0,0,KSN_PROC_W,KSN_PROC_H};
    int x0=b.x0,y0=b.y0,x1=b.x1,y1=b.y1;
    damage_add(surface->candidate,&x0,&y0,&x1,&y1);
    /* VM and typed segments may lie anywhere in -480..720. Clamping each edge
     * keeps x0<=x1 and y0<=y1 for any non-empty set; a set wholly off one side
     * collapses to a zero-width rectangle on that edge. */
    if(x0<0)x0=0;
    if(y0<0)y0=0;
    if(x1>KSN_PROC_W)x1=KSN_PROC_W;
    if(y1>KSN_PROC_H)y1=KSN_PROC_H;
    return (ksn_rect){(int16_t)x0,(int16_t)y0,(int16_t)x1,(int16_t)y1};
}
#ifdef POCKET_PROC_SIG_CHECK
/* Host check of the band hash (docs/platform/lcd-dma-driver.md 9.10): a
 * splitmix64 chain over the same bytes stands in for "the entries differ". */
static uint64_t sig_mix64(uint64_t z){
    z+=0x9e3779b97f4a7c15ull;z=(z^(z>>30))*0xbf58476d1ce4e5b9ull;
    z=(z^(z>>27))*0x94d049bb133111ebull;return z^(z>>31);
}
#include <stdio.h>
static unsigned long long sig_compared,sig_changed,sig_coll32,sig_lo16,sig_hi16,sig_lo8,sig_impossible,
                          sig_skipped,sig_pixel_bad,sig_all;
/* The committed frame as it was before commit() merged over it, so a
 * skipped band is checked against the frame on the panel. */
static ksn_proc_frame *sig_old;
static unsigned long long one_merges,one_spare_need;
__attribute__((destructor)) static void one_report(void){
    if(!one_merges)return;
    fprintf(stderr,"ONE_BUFFER merges=%llu spare_needed=%llu\n",one_merges,one_spare_need);
    if(getenv("POCKET_ONE_REPORT")){
        FILE *f=fopen(getenv("POCKET_ONE_REPORT"),"a");
        if(f){fprintf(f,"spare_needed=%llu merges=%llu\n",one_spare_need,one_merges);fclose(f);}
    }
}
__attribute__((destructor)) static void sig_report(void){
    if(!sig_compared&&!sig_all)return;
    fprintf(stderr,"SIG_CHECK bands_compared=%llu changed=%llu fnv32_equal_when_changed=%llu "
            "lo16=%llu (ideal %.1f) hi16=%llu (ideal %.1f) lo8=%llu (ideal %.1f) impossible=%llu "
            "skipped=%llu pixel_mismatch_in_skipped=%llu whole_frame_damages=%llu\n",
            sig_compared,sig_changed,sig_coll32,sig_lo16,sig_changed/65536.0,sig_hi16,sig_changed/65536.0,
            sig_lo8,sig_changed/256.0,sig_impossible,sig_skipped,sig_pixel_bad,sig_all);
}
#endif
/* One entry (or a pattern line's two) as 32 bits, every input bit reaching
 * every output bit -- the band step below relies on that. proc_band_hash.h:
 * two products on the 32 x 32 -> 64 multiplier a segment instead of
 * murmur3's nine multiplies and rotations (docs/vm/qjs-observe.md sec.22). */
static inline __attribute__((always_inline)) uint32_t rotl32(uint32_t v,unsigned r){return v<<r|v>>(32u-r);}
static inline __attribute__((always_inline)) uint32_t entry_hash(const ksn_proc_segment *s,unsigned n){
    return proc_band_entry_hash(s,n);
}
/* The band's running value and one entry's: order matters (the rotation),
 * and the rotation brings the product's high bits back to the low ones, the
 * weak end of a plain multiply (the FNV-1a check, sec.9.10). */
static inline __attribute__((always_inline)) uint32_t band_mix(uint32_t h,uint32_t e){
    return (rotl32(h,15)^e)*0x9e3779b1u;
}
/* One entry (or a pattern line's two, [start, i)) into each band it touches. */
static inline __attribute__((always_inline)) void sig_add(const ksn_proc_frame *frame,proc_band_sig *sig,
                                                          const ksn_proc_segment *geometry,
                                                          unsigned start,unsigned i){
    const int ya=geometry->y0<geometry->y1?geometry->y0:geometry->y1,yb=geometry->y0^geometry->y1^ya;
    int xa=geometry->x0<geometry->x1?geometry->x0:geometry->x1,xb=(geometry->x0^geometry->x1^xa)+1;
    if(yb<0||ya>=KSN_PROC_H||xb<=0||xa>=KSN_PROC_W)return;
    if(xa<0)xa=0;
    if(xb>KSN_PROC_W)xb=KSN_PROC_W;
    const unsigned b0=(unsigned)(ya<0?0:ya)/8u,b1=(unsigned)(yb>=KSN_PROC_H?KSN_PROC_H-1:yb)/8u;
    /* The raw entries, a pattern line's parameters included, hashed once;
     * each band they touch then takes one mixing step. Hashing the bytes
     * again for every band was most of commit() on the device
     * (docs/platform/lcd-dma-driver.md 9.11). */
    const uint32_t e=entry_hash(&frame->segments[start],i-start);
    for(unsigned b=b0;b<=b1;b++){
        sig->hash[b]=band_mix(sig->hash[b],e);
#ifdef POCKET_PROC_SIG_CHECK
        const uint8_t *bytes=(const uint8_t *)&frame->segments[start];
        for(size_t k=0;k<(size_t)(i-start)*sizeof frame->segments[0];k++)
            sig->strong[b]=sig_mix64(sig->strong[b]^bytes[k]);
#endif
        if(xa<sig->x0[b])sig->x0[b]=(uint8_t)xa;
        if(xb>sig->x1[b])sig->x1[b]=(uint8_t)xb;
        if(sig->first[b]==UINT16_MAX)sig->first[b]=(uint16_t)start;
        sig->end[b]=(uint16_t)i;
    }
}
PROC_HOT static void band_signature(const ksn_proc_frame *frame,proc_band_sig *sig){
    for(unsigned b=0;b<PROC_BANDS;b++){
        sig->hash[b]=2166136261u;sig->x0[b]=255;sig->x1[b]=0;
        sig->first[b]=UINT16_MAX;sig->end[b]=0;
#ifdef POCKET_PROC_SIG_CHECK
        sig->strong[b]=0x9e3779b97f4a7c15ull;
#endif
    }
    if(!frame->ext){
        for(unsigned i=0;i<frame->count;i++)sig_add(frame,sig,&frame->segments[i],i,i+1);
        return;
    }
    unsigned i=0,cost;
    ksn_proc_segment geometry;
    bool pattern;
    for(;;){
        const unsigned start=i;
        if(!ksn_proc_frame_next(frame,&i,&geometry,&pattern,&cost))break;
        sig_add(frame,sig,&geometry,start,i);
    }
}
#ifdef POCKET_PROC_VISIT_STATS
/* Host diagnostic (docs/platform/lcd-dma-driver.md 9.5): for each committed
 * frame, how many entries a renderer looks at to draw every 8-row band and
 * every 4-row piece -- all of them (today), only those between the first and
 * the last entry touching it (design B), only those touching it (design A). */
unsigned long long pocket_proc_visits[3][2];   /* [all,B,A][8-row,4-row] */
unsigned pocket_proc_visit_frames,pocket_proc_list_max[2],pocket_proc_entries_max;
static void visit_stats(const ksn_proc_frame *frame){
    enum{PIECES=(KSN_PROC_H+3)/4};
    int first[PIECES],last[PIECES];unsigned touch[PIECES]={0},touchb[PROC_BANDS]={0};
    for(unsigned p=0;p<PIECES;p++){first[p]=-1;last[p]=-1;}
    unsigned i=0,cost,entries=0;
    ksn_proc_segment g;bool pattern;
    for(;;){
        const unsigned start=i;
        if(!ksn_proc_frame_next(frame,&i,&g,&pattern,&cost))break;
        entries++;
        const int ya=g.y0<g.y1?g.y0:g.y1,yb=g.y0^g.y1^ya;
        if(yb<0||ya>=KSN_PROC_H)continue;
        const unsigned p0=(unsigned)(ya<0?0:ya)/4u,p1=(unsigned)(yb>=KSN_PROC_H?KSN_PROC_H-1:yb)/4u;
        for(unsigned p=p0;p<=p1;p++){
            if(first[p]<0)first[p]=(int)start;
            last[p]=(int)start;touch[p]++;
        }
        for(unsigned b=p0/2u;b<=p1/2u;b++)touchb[b]++;
    }
    pocket_proc_visit_frames++;
    if(entries>pocket_proc_entries_max)pocket_proc_entries_max=entries;
    unsigned list4=0,list8=0;
    for(unsigned p=0;p<PIECES;p++)list4+=touch[p];
    for(unsigned b=0;b<PROC_BANDS;b++)list8+=touchb[b];
    if(list4>pocket_proc_list_max[1])pocket_proc_list_max[1]=list4;
    if(list8>pocket_proc_list_max[0])pocket_proc_list_max[0]=list8;
    for(unsigned p=0;p<PIECES;p++){
        pocket_proc_visits[0][1]+=entries;
        if(first[p]>=0)pocket_proc_visits[1][1]+=(unsigned)(last[p]-first[p]+1);
        pocket_proc_visits[2][1]+=touch[p];
    }
    for(unsigned b=0;b<PROC_BANDS;b++){
        const unsigned pa=2*b,pb=2*b+1<PIECES?2*b+1:2*b;
        int f=first[pa],l=last[pa];
        if(first[pb]>=0&&(f<0||first[pb]<f))f=first[pb];
        if(last[pb]>l)l=last[pb];
        pocket_proc_visits[0][0]+=entries;
        if(f>=0)pocket_proc_visits[1][0]+=(unsigned)(l-f+1);
        pocket_proc_visits[2][0]+=touchb[b];
    }
}
#endif
/* Draw rows y..y+rows-1 of one of a surface's frames, walking only the
 * entries its signature says can touch them. A frame without a signature
 * that describes it (none yet) takes the whole walk; the pixels are the same
 * either way. g_pocket_proc_ranges 0 is the whole walk always (A/B). */
int g_pocket_proc_ranges=1;
static bool render_rows(const proc_surface *surface,const ksn_proc_frame *frame,
                        uint16_t *pixels,int y,int rows){
    const proc_band_sig *sig=NULL;
    if(g_pocket_proc_ranges&&rows>0){
        if(frame==surface->candidate&&surface->sig_ready)sig=&surface->candidate_sig;
        else if(frame==surface->committed&&surface->has_committed)sig=&surface->committed_sig;
    }
    if(!sig)return ksn_proc_render_band(frame,pixels,y,rows);
    unsigned first=UINT16_MAX,end=0;
    for(unsigned b=(unsigned)y/8u;b<=(unsigned)(y+rows-1)/8u&&b<PROC_BANDS;b++){
        if(sig->first[b]>sig->end[b])continue;
        if(sig->first[b]<first)first=sig->first[b];
        if(sig->end[b]>end)end=sig->end[b];
    }
    if(first>end)first=end=0;
    /* half_carry: taken by pocket_proc_prepare_render, never here -- this
     * may run on the display task, which allocates nothing (5d). */
    return ksn_proc_render_band_carry(frame,pixels,y,rows,first,end,half_carry);
}
bool pocket_proc_band_damage(unsigned index,uint32_t *bands,int16_t x0[],int16_t x1[]){
#ifdef POCKET_PROC_NO_BAND_DAMAGE
    /* Host A/B only (run_megademo_app_host.py --no-band-damage). */
    (void)bands;(void)x0;(void)x1;(void)index;
    return false;
#endif
    if(index>=PROC_SURFACES||!surfaces[index].sig_ready)return false;
    const proc_surface *s=&surfaces[index];
    const bool all=!s->has_committed||s->candidate_color!=s->committed_color;
#ifdef POCKET_PROC_SIG_CHECK
    if(all)sig_all++;
    else for(unsigned b=0;b<PROC_BANDS;b++){
        const proc_band_sig *n=&s->candidate_sig,*o=&s->committed_sig;
        const uint32_t hn=n->hash[b],ho=o->hash[b];
        const bool fe=hn==ho,se=n->strong[b]==o->strong[b];
        sig_compared++;
        if(se&&!fe)sig_impossible++;
        if(!se){
            sig_changed++;
            if(fe)sig_coll32++;
            if(!((hn^ho)&0xffffu))sig_lo16++;
            if((hn>>16)==(ho>>16))sig_hi16++;
            if(!((hn^ho)&0xffu))sig_lo8++;
        }
        if(fe){
            /* Skipped: both frames must draw the band alike over the colour. */
            static uint16_t pa[KSN_PROC_W*8],pb[KSN_PROC_W*8];
            const int y=(int)b*8,rows=y+8>KSN_PROC_H?KSN_PROC_H-y:8;
            for(unsigned k=0;k<KSN_PROC_W*8u;k++)pa[k]=pb[k]=s->candidate_color;
            /* The candidate is marked ready only when it is published. */
            static ksn_proc_frame copy;
            memcpy(&copy,s->candidate,frame_bytes(surface_segments(s)));copy.ready=true;
            const bool ra=ksn_proc_render_band(&copy,pa,y,rows);
            const bool rb=ksn_proc_render_band(sig_old?sig_old:s->committed,pb,y,rows);
            sig_skipped++;
            if(memcmp(pa,pb,sizeof pa)){
                if(sig_pixel_bad<3)fprintf(stderr,"SIG_BAD band=%u ra=%d rb=%d ready=%d/%d count=%u/%u ext=%u/%u first=%u/%u end=%u/%u\n",
                    b,ra,rb,s->candidate->ready,s->committed->ready,s->candidate->count,s->committed->count,
                    s->candidate->ext,s->committed->ext,n->first[b],o->first[b],n->end[b],o->end[b]);
                sig_pixel_bad++;
            }
        }
    }
#endif
    *bands=0;
    for(unsigned b=0;b<PROC_BANDS;b++){
        const proc_band_sig *n=&s->candidate_sig,*o=&s->committed_sig;
        if(!all&&n->hash[b]==o->hash[b])continue;
        *bands|=1u<<b;
        if(all){x0[b]=0;x1[b]=KSN_PROC_W;continue;}
        /* The columns either frame drew on the band. */
        const unsigned a=n->x0[b]<o->x0[b]?n->x0[b]:o->x0[b],z=n->x1[b]>o->x1[b]?n->x1[b]:o->x1[b];
        x0[b]=(int16_t)(a<z?a:0);x1[b]=(int16_t)(a<z?z:0);
    }
    return true;
}
/* ---- The frame being built (proc_surface's note) */
/* Where the next entries go and how many fit there: behind the committed
 * frame, never past the frame's own limit. *frame_room: the frame's. */
static ksn_proc_segment *write_at(const proc_surface *s,unsigned *room,unsigned *frame_room){
    const unsigned place=surface_place(s)-s->tail_base-s->tail_used;
    *frame_room=surface_segments(s)-s->tail_used;
    *room=place<*frame_room?place:*frame_room;
    return &s->candidate->segments[s->tail_base+s->tail_used];
}
/* n entries just written at write_at(). */
static void wrote(proc_surface *s,unsigned n,unsigned raster,bool ext){
    s->tail_used=(uint16_t)(s->tail_used+n);
    s->tail_raster=(uint16_t)(s->tail_raster+raster);
    if(ext)s->tail_ext=1;
}
/* The frame fits its limit but not, with the committed one, the buffer:
 * the spare is too small for this app (resource({spare})). */
static const char spare_limit[]="frame drawing limit (the committed frame and this one exceed maxSegments + spare)";
static JSValue begin_impl(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *op="kasane.shiki.beginFrame";
    uint32_t color;
    unsigned index=0;
    if((argc!=1&&argc!=2)||!integer(ctx,argv[0],UINT16_MAX,&color)||
       (argc==2&&!surface_index(ctx,argv[1],&index)))
        return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"expected RGB565 and optional live surface ID");
    /* A pending frame that is on its way to the panel (a present in flight,
     * P2a) does not hold the next one up: the new frame is built behind it
     * (the tail) and commit() joins before it touches the front. */
    if((any_pending()&&!disp_busy())||any_repair())
        return failure(ctx,op,POCKET_ERR_BUSY,"previous frame awaits presentation or repair");
    proc_surface *surface=&surfaces[index];
    if(!buffers(surface))return failure(ctx,op,POCKET_ERR_OUT_OF_MEMORY,"frame allocation failed");
    /* The committed entries stay where they are (and so does the header,
     * theirs until commit()); the new frame goes behind them. */
    /* Even, so the tail and the front share their alignment mod 4 (an
     * entry is 10 B): commit()'s copy then moves words, not bytes. */
    /* Behind whichever frame is at the front: the committed one, or a
     * pending one still on its way to the panel (5b). */
    unsigned base=(surface->has_committed||surface->pending?surface->candidate->count+1u:0u)&~1u;
    if(base>surface_place(surface))base=surface_place(surface);
    surface->tail_base=(uint16_t)base;
    surface->tail_used=surface->tail_raster=0;
    surface->tail_ext=0;
    surface->tail_color=(uint16_t)color;
    building_surface=index;
    building=true;
    return JS_UNDEFINED;
}
/* Native drawing straight into the candidate (pocket_shape.c): draw()'s
 * frame limits without a plan or the VM. */
JSValue pocket_proc_native_begin(JSContext *ctx,const char *op,pocket_proc_mark *mark){
    if(js_call_active)return failure(ctx,"kasane.shiki",POCKET_ERR_BUSY,"reentrant drawing call");
    if(!building)return failure(ctx,op,POCKET_ERR_BUSY,"beginFrame required");
    const proc_surface *s=&surfaces[building_surface];
    *mark=(pocket_proc_mark){s->tail_used,s->tail_raster,s->tail_ext,(uint8_t)building_surface};
    return JS_UNDEFINED;
}
JSValue pocket_proc_native_line(JSContext *ctx,const char *op,int x0,int y0,int x1,int y1,uint16_t color){
    if(!building)return failure(ctx,op,POCKET_ERR_BUSY,"beginFrame required");
    if(x0< -480||x0>720||y0< -480||y0>720||x1< -480||x1>720||y1< -480||y1>720){
        building=false;
        return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"segment outside -480..720");
    }
    proc_surface *s=&surfaces[building_surface];
    const unsigned dx=(unsigned)abs(x1-x0),dy=(unsigned)abs(y1-y0),cost=(dx>dy?dx:dy)+1u;
    unsigned room,frame_room;
    ksn_proc_segment *at=write_at(s,&room,&frame_room);
    if(!room||cost>(unsigned)UINT16_MAX-s->tail_raster){
        building=false;
        return failure(ctx,op,POCKET_ERR_LIMIT_EXCEEDED,
                       frame_room&&cost<=(unsigned)UINT16_MAX-s->tail_raster?spare_limit:"frame drawing limit");
    }
    *at=(ksn_proc_segment){(int16_t)x0,(int16_t)y0,(int16_t)x1,(int16_t)y1,color};
    wrote(s,1,cost,false);
    return JS_UNDEFINED;
}
void pocket_proc_native_rollback(const pocket_proc_mark *mark){
    proc_surface *s=&surfaces[mark->surface<PROC_SURFACES?mark->surface:0];
    if(!s->candidate)return;
    s->tail_used=mark->count;s->tail_raster=mark->raster;s->tail_ext=mark->ext;
}
#ifdef POCKET_PROC_SIG_CHECK
static void sig_keep_old(const proc_surface *s){
    free(sig_old);sig_old=NULL;
    if(!s->has_committed)return;
    const size_t bytes=frame_bytes(surface_segments(s));
    if((sig_old=malloc(bytes)))memcpy(sig_old,s->committed,bytes);
}
#endif
/* n entries from src to dst, front to back (dst <= src when they overlap),
 * both on the same alignment mod 4 (tail_base is even, so they are):
 * four words a step. The C library's memmove/memcpy took ~1.8 instructions
 * a byte here on r5sim (STRESS: 8.9 KB a frame, ~2,500 counts a turn). */
typedef uint32_t proc_word __attribute__((may_alias));
typedef uint16_t proc_half __attribute__((may_alias));
static void copy_entries(ksn_proc_segment *dst,const ksn_proc_segment *src,unsigned n){
    uint8_t *d=(uint8_t *)dst;
    const uint8_t *q=(const uint8_t *)src;
    size_t len=(size_t)n*sizeof *dst;
    if(!len||d==q)return;
    if(((uintptr_t)d^(uintptr_t)q)&3u){memmove(d,q,len);return;}
    if((uintptr_t)d&2u){
        *(proc_half *)(void *)d=*(const proc_half *)(const void *)q;
        d+=2;q+=2;len-=2;
    }
    proc_word *w=(proc_word *)(void *)d;
    const proc_word *v=(const proc_word *)(const void *)q;
    size_t k=0,words=len/4u;
    for(;k+4u<=words;k+=4u){
        const uint32_t a=v[k],b=v[k+1],c=v[k+2],e=v[k+3];
        w[k]=a;w[k+1]=b;w[k+2]=c;w[k+3]=e;
    }
    for(;k<words;k++)w[k]=v[k];
    if(len&2u)*(proc_half *)(void *)(d+(len&~(size_t)3u))=*(const proc_half *)(const void *)(q+(len&~(size_t)3u));
}
/* The new frame to the front of the buffer, over the committed one. */
static void merge(proc_surface *s){
    ksn_proc_frame *f=s->candidate;
    /* The front frame changes here and only here: what describes or caches
     * it goes stale now. Drawing writes the tail alone, so the display may
     * read the front, its colour, signature and carry until this point (5c). */
    s->sig_ready=false;
    s->candidate_color=s->tail_color;
    image_band_valid=false;carry_drop();
    s->committed_lost=s->has_committed;
    copy_entries(f->segments,&f->segments[s->tail_base],s->tail_used);
    f->count=s->tail_used;f->raster_steps=s->tail_raster;f->ext=s->tail_ext;f->ready=false;
#ifdef POCKET_PROC_SIG_CHECK
    one_merges++;
    {
        const long need=(long)s->tail_base+(long)s->tail_used-(long)surface_segments(s);
        if(need>(long)one_spare_need)one_spare_need=(unsigned long long)need;
    }
#endif
}
static JSValue commit_impl(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argv;
    const char *op="kasane.shiki.commit";
    frame_bd_join_point(FRAME_BD_JOIN_SHIKI_COMMIT);
    if(argc||!building)return failure(ctx,op,POCKET_ERR_BUSY,"beginFrame required");
    /* commit joined before it ran (it is marked): the frame beginFrame let
     * through as in flight has been acknowledged by now. If it did not reach
     * the panel (a transfer failure leaves it pending or asks for repair),
     * merge() must not overwrite its front (two-core-pipeline.md sec.7.2 F8). */
    if(any_pending()||any_repair()){
        building=false;
        return failure(ctx,op,POCKET_ERR_BUSY,"previous frame awaits presentation or repair");
    }
    proc_surface *surface=&surfaces[building_surface];
    if(building_surface&&!surface->image_mode)
        return failure(ctx,op,POCKET_ERR_BUSY,"resource(surface ID) required");
    const proc_box old=old_box(surface);
#ifdef POCKET_PROC_SIG_CHECK
    sig_keep_old(surface);
#endif
    merge(surface);
    surface->damage=frame_damage(surface,old);
    band_signature(surface->candidate,&surface->candidate_sig);
    surface->sig_ready=true;
#ifdef POCKET_PROC_VISIT_STATS
    visit_stats(surface->candidate);
#endif
    ksn_result result=building_surface||surface->image_mode?
        pocket_kasane_proc_publish_at(building_surface,surface->damage):
        pocket_kasane_proc_publish();
    if(result!=KSN_OK){
        /* The new frame is at the front now and the committed one is gone:
         * the surface shows nothing until its next commit. */
        building=false;surface->sig_ready=false;
        if(surface->committed_lost){
            surface->committed_lost=false;surface->has_committed=false;
            if(!surface->image_mode)pocket_kasane_invalidate();
        }
        surface->candidate->count=0;
        return failure(ctx,op,result==KSN_OOM?POCKET_ERR_OUT_OF_MEMORY:
                       POCKET_ERR_BUSY,"Kasane presentation unavailable");
    }
    surface->candidate->ready=true;
    surface->pending=true;
    pending_surface=building_surface;
    image_band_valid=false;carry_drop();
    building=false;
    /* A frame that changed nothing invalidates nothing (publish_at gets empty
     * damage), and with no other Kasane work no present runs to acknowledge
     * it: pending would stay set and every later beginFrame fail BUSY. Seen
     * in r5sim (STRESS LV3, docs/vm/lv3-heap-exhaustion.md): an OOM turn whose
     * HUD patch failed committed an unchanged frame, and the app never drew
     * again. Nothing to show, so it counts as shown. */
    if(!pocket_kasane_needs_present())pocket_proc_present_result(KSN_OK);
#ifdef KASANE_MEGADEMO_TRACE
    trace.commit_n++;trace_split=true;
#endif
    return JS_UNDEFINED;
}
typedef JSValue (*proc_js_method)(JSContext *,JSValueConst,int,JSValueConst *);
static JSValue guarded(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv,
                       proc_js_method method){
    if(js_call_active)
        return failure(ctx,"kasane.shiki",POCKET_ERR_BUSY,"reentrant drawing call");
    js_call_active=true;
    JSValue result=method(ctx,self,argc,argv);
    js_call_active=false;
    return result;
}
static JSValue js_begin(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    return guarded(ctx,self,argc,argv,begin_impl);
}
#ifdef POCKET_PROC_DRAW_HOOK
bool pocket_proc_hook_dry;
#endif
/* One draw for a native caller (pocket_derby.c): a Shiki handle, the inputs
 * converted by sec.2.4 of the spec. */
JSValue pocket_proc_draw_numbers(JSContext *ctx,uint32_t handle,const double *in,unsigned n){
    const char *op="kasane.shiki.draw";
#ifdef POCKET_PROC_DRAW_HOOK
    if(pocket_proc_hook_dry){pocket_proc_draw_hook(handle,in,n);return JS_UNDEFINED;}
#endif
    if(handle>INT32_MAX||n>KSN_SHIKI_INPUTS)
        return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"expected handle and 0..8 inputs");
    JSValue result=pocket_proc_shiki_draw_inputs(ctx,handle,in,NULL,n,0);
#ifdef POCKET_PROC_DRAW_HOOK
    if(!JS_IsException(result))pocket_proc_draw_hook(handle,in,n);
#endif
    return result;
}
static JSValue js_commit(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    return guarded(ctx,self,argc,argv,commit_impl);
}
/* {maxSegments: 1..1024} (absent or undefined: 0, the 1,024 default). */
static bool segments_option(JSContext *ctx,JSValueConst object,uint32_t *segments){
    *segments=0;
    JSValue v=JS_GetPropertyStr(ctx,object,"maxSegments");
    bool ok=!JS_IsException(v)&&(JS_IsUndefined(v)||
            (integer(ctx,v,KSN_PROC_SEGMENTS,segments)&&*segments>0));
    JS_FreeValue(ctx,v);
    if(!ok)*segments=0;
    return ok;
}
/* {spare: 0..1024}: the entries the buffer holds past maxSegments, for the
 * committed frame and the next to share it (proc_surface's note). Absent
 * or undefined: *set false, as many as maxSegments. */
static bool spare_option(JSContext *ctx,JSValueConst object,uint32_t *spare,bool *set){
    *spare=0;*set=false;
    JSValue v=JS_GetPropertyStr(ctx,object,"spare");
    bool ok=!JS_IsException(v);
    if(ok&&!JS_IsUndefined(v)){ok=integer(ctx,v,KSN_PROC_SEGMENTS,spare);*set=ok;}
    JS_FreeValue(ctx,v);
    if(!ok)*spare=0;
    return ok;
}
static JSValue resource_impl(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *op="kasane.shiki.resource";
    unsigned index=0;
    /* resource({maxSegments, spare}): surface 0's image, its buffer sized
     * for maxSegments + spare (createSurface()'s options for surface 1).
     * Decided before surface 0's first beginFrame allocates it; after that
     * only the same values are accepted. */
    if(argc==1&&JS_IsObject(argv[0])){
        uint32_t segments=0,spare=0;
        bool spare_set=false;
        if(!segments_option(ctx,argv[0],&segments))
            return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"maxSegments must be an integer 1..1024");
        if(!spare_option(ctx,argv[0],&spare,&spare_set))
            return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"spare must be an integer 0..1024");
        const unsigned want=segments?segments:KSN_PROC_SEGMENTS;
        if(surfaces[0].candidate&&(surface_segments(&surfaces[0])!=want||
                                   surface_place(&surfaces[0])!=want+(spare_set?spare:want)))
            return failure(ctx,op,POCKET_ERR_BUSY,"surface 0's frames are already allocated");
        surfaces[0].max_segments=(uint16_t)segments;
        surfaces[0].spare=(uint16_t)spare;surfaces[0].spare_set=spare_set;
        return pocket_kasane_proc_resource(ctx);
    }
    if(argc>1||(argc==1&&!surface_index(ctx,argv[0],&index)))
        return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,
                       "expected optional live surface ID or {maxSegments}");
    return index?pocket_kasane_proc_resource_at(ctx,index):pocket_kasane_proc_resource(ctx);
}
static JSValue js_resource(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    return guarded(ctx,self,argc,argv,resource_impl);
}
/* createSurface() or createSurface({maxSegments: 1..1024, spare: 0..1024}).
 * maxSegments only shrinks the surface's frame; it is enforced by draw()
 * exactly as the 1,024 default is (LIMIT_EXCEEDED "frame drawing limit",
 * candidate kept short of the draw). spare sizes the buffer behind it
 * (proc_surface's note). Both are decided before the surface exists: a bad
 * option issues no handle. */
static JSValue create_surface_impl(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *op="kasane.shiki.createSurface";
    uint32_t segments=0,spare=0;
    bool spare_set=false;
    if(argc>1)return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,
                             "expected optional {maxSegments, spare}");
    if(argc==1&&!JS_IsUndefined(argv[0])){
        if(!JS_IsObject(argv[0]))
            return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"expected optional {maxSegments, spare}");
        if(!segments_option(ctx,argv[0],&segments))
            return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,
                           "maxSegments must be an integer 1..1024");
        if(!spare_option(ctx,argv[0],&spare,&spare_set))
            return failure(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"spare must be an integer 0..1024");
    }
    if(surfaces[1].handle||next_surface_handle==INT32_MAX)
        return failure(ctx,op,POCKET_ERR_LIMIT_EXCEEDED,"two surfaces maximum");
    surfaces[1].max_segments=(uint16_t)segments;
    surfaces[1].spare=(uint16_t)spare;surfaces[1].spare_set=spare_set;
    surfaces[1].handle=++next_surface_handle;
    return JS_NewInt32(ctx,(int32_t)surfaces[1].handle);
}
static JSValue js_create_surface(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    return guarded(ctx,self,argc,argv,create_surface_impl);
}
/* ---- Shiki programs (pocket.kasane.shiki, pocket_shiki_api.c) */
uint32_t pocket_proc_shiki_attach(void *plan){
    unsigned slot=0;
    while(slot<PROC_HANDLES&&slots[slot].plan)slot++;
    if(slot==PROC_HANDLES||next_handle==PROC_HANDLE_MAX)return 0;
    const uint32_t handle=++next_handle;
    slots[slot]=(proc_slot){.handle=handle,.plan=plan};
    return handle;
}
void *pocket_proc_shiki_plan(uint32_t handle){
    proc_slot *slot=find_slot(handle);
    return slot?slot->plan:NULL;
}
bool pocket_proc_shiki_detach(uint32_t handle){
    /* As unregister(): legal between beginFrame() and commit(), since draw
     * copies the segments out and keeps nothing of the plan. */
    proc_slot *slot=find_slot(handle);
    if(!slot)return false;
    ksn_shiki_plan_dispose(slot->plan);
    shiki_or_sys_free(slot->plan);
    *slot=(proc_slot){0};
    return true;
}
pocket_shiki_draw_prof pocket_shiki_prof;
static pocket_shiki_turn shiki_turn;
void pocket_shiki_turn_take(pocket_shiki_turn *out){*out=shiki_turn;shiki_turn=(pocket_shiki_turn){0};}
uint32_t pocket_cycles(void){
#ifdef ESP_PLATFORM
    return esp_cpu_get_cycle_count();
#else
    return 0;
#endif
}
JSValue pocket_proc_shiki_draw(JSContext *ctx,uint32_t handle,const double *in,unsigned n){
    return pocket_proc_shiki_draw_inputs(ctx,handle,in,NULL,n,0);
}
JSValue pocket_proc_shiki_draw_inputs(JSContext *ctx,uint32_t handle,const double *in,const int32_t *fixed,unsigned n,
                                      unsigned index){
    const char *op="kasane.shiki.draw";
    const uint32_t t_in=pocket_cycles();
    if(js_call_active)return failure(ctx,"kasane.shiki",POCKET_ERR_BUSY,"reentrant drawing call");
    if(!building)return failure(ctx,op,POCKET_ERR_BUSY,"beginFrame required");
    proc_slot *slot=find_slot(handle);
    if(!slot)return failure(ctx,op,POCKET_ERR_CLOSED,"stale Shiki handle");
    if(!shiki_vm&&!(shiki_vm=shiki_or_sys_calloc(1,sizeof *shiki_vm))){
        building=false;
        return failure(ctx,op,POCKET_ERR_LIMIT_EXCEEDED,"no room for the engine's state");
    }
    proc_surface *surface=&surfaces[building_surface];
    pocket_shiki_draw_prof *pf=&pocket_shiki_prof;
    if(pf->on){const uint32_t t=pocket_cycles();pf->lookup+=t-pf->mark;pf->mark=t;}
    /* Real inputs: sec.2.4's conversion (ksn_shiki_draw's), then the draw by
     * its number (DRAW_INDEX, Shiki v2.0 draft sec.4.1). */
    const ksn_shiki_plan *pl=slot->plan;
    int32_t fx[KSN_SHIKI_INPUTS]={0};
    const int32_t *args=fixed;
    unsigned nargs=n;
    bool good=true;
    if(!fixed){
        for(unsigned k=0;k<pl->inputs&&good;k++)
            good=ksn_shiki_from_double(k<n?in[k]:0.0,pl->input_q[k],&fx[k]);
        args=fx;nargs=pl->inputs;
    }
    /* Straight into the frame's place: a draw that does not fit stops at
     * LIMIT and nothing is counted, so its segments are not the frame's. */
    ksn_shiki_status st;
    unsigned room,frame_room;
    ksn_proc_segment *at=write_at(surface,&room,&frame_room);
    shiki_turn.draws++;
    const uint32_t t_engine=pocket_cycles();
    if(good)st=ksn_shiki_draw_index_cap(pl,args,nargs,index,shiki_vm,at,room);
    else{memset(shiki_vm,0,sizeof *shiki_vm);st=KSN_SHIKI_BADINPUT;}
    shiki_turn.engine_cy+=pocket_cycles()-t_engine;
    shiki_turn.steps+=shiki_vm->steps;
    /* A pattern line needs two entries: one left reads as full too. */
    const bool full=st==KSN_SHIKI_LIMIT&&room<KSN_SHIKI_SEGMENTS&&shiki_vm->count+1u>=room;
    if(pf->on){const uint32_t t=pocket_cycles();pf->engine+=t-pf->mark;pf->mark=t;}
    /* Out of the frame's room reads as the frame's limit, as when the room
     * was checked after the draw; the engine's own per-draw limits as its. */
    if(st!=KSN_SHIKI_DONE&&!full){
        building=false;
        return failure(ctx,op,st==KSN_SHIKI_LIMIT?POCKET_ERR_LIMIT_EXCEEDED:POCKET_ERR_INVALID_ARGUMENT,
                       st==KSN_SHIKI_BADINPUT?"non-finite input":"Shiki execution failed");
    }
    if(full||(unsigned)shiki_vm->raster>(unsigned)UINT16_MAX-surface->tail_raster){
        building=false;
        return failure(ctx,op,POCKET_ERR_LIMIT_EXCEEDED,full&&room<frame_room?spare_limit:"frame drawing limit");
    }
    wrote(surface,shiki_vm->count,shiki_vm->raster,shiki_vm->ext);
    if(pf->on){pf->copy+=pocket_cycles()-pf->mark;pf->n++;}
    shiki_turn.total_cy+=pocket_cycles()-t_in;
    return JS_UNDEFINED;
}

// The frame's functions on kasane.shiki (pocket_shiki_api.c).
// beginFrame writes only the tail, so it does not join; the others change
// what a present reads and do (two-core-pipeline.md sec.2.3 A).
static const JSCFunctionListEntry begin_method[]={
    JS_CFUNC_DEF("beginFrame",1,js_begin),
};
static const JSCFunctionListEntry frame_methods[]={
    JS_CFUNC_DEF("commit",0,js_commit),
    JS_CFUNC_DEF("resource",0,js_resource),
    JS_CFUNC_DEF("createSurface",0,js_create_surface),
};
esp_err_t pocket_proc_frame_install(JSContext *ctx,JSValueConst obj){
    JSCFunctionMark mark=pocket_nojoin_begin(ctx);
    int r=JS_SetPropertyFunctionList(ctx,obj,begin_method,1);
    pocket_nojoin_end(ctx,mark);
    if(r<0)return ESP_ERR_NO_MEM;
    return JS_SetPropertyFunctionList(ctx,obj,frame_methods,
                                      (int)(sizeof frame_methods/sizeof frame_methods[0]))<0?ESP_ERR_NO_MEM:ESP_OK;
}
void pocket_proc_reset(void){
    for(unsigned i=0;i<PROC_HANDLES;i++){
        if(slots[i].plan)ksn_shiki_plan_dispose(slots[i].plan);
        shiki_or_sys_free(slots[i].plan);slots[i]=(proc_slot){0};
    }
    for(unsigned i=0;i<PROC_SURFACES;i++){
        shiki_or_sys_free(surfaces[i].candidate);   /* committed is the same buffer */
        surfaces[i]=(proc_surface){0};
    }
    shiki_or_sys_free(image_band_buf);image_band_buf=NULL;
    shiki_or_sys_free(half_carry);half_carry=NULL;
    shiki_or_sys_free(shiki_vm);shiki_vm=NULL;
    image_band_valid=false;carry_drop();image_band_frame=NULL;
    building=js_call_active=false;building_surface=pending_surface=0;
#ifdef KASANE_PROC_JS_DIAGNOSTIC
    image_band_count=image_band_cycles=image_span_count=image_span_cycles=0;
#endif
#ifdef KASANE_MEGADEMO_TRACE
    trace=(pocket_proc_trace){0};trace_split=false;
#endif
}
void pocket_proc_end_turn(void){
    /* A thrown JS frame must not carry an unfinished candidate into the next
     * owner turn. Previously committed or pending presentation stays intact.
     * Not called at the end of a host turn that the VM parked: that frame()
     * resumes in the continuation and still owns its candidate
     * (pocket_kasane_park_turn, end_guest_turn in app_session.c). */
    building=false;
}
/* Before a present: the first one drawn in pieces shorter than a band takes
 * the carry, so a port of 8-row strips never allocates it and beginFrame
 * still takes exactly its two frames (test_pocket_proc_turn_qjs.c). No room
 * is no carry. On the UI task: the render that uses it may not be (5d). */
void pocket_proc_prepare_render(unsigned strip_rows){
    if(half_carry||strip_rows>=8)return;
    for(unsigned i=0;i<PROC_SURFACES;i++)if(surfaces[i].candidate){
        half_carry=shiki_or_sys_calloc(1,sizeof *half_carry);
        return;
    }
}
bool pocket_proc_has_frame(void){
    const proc_surface *s=&surfaces[0];
    return s->pending||s->has_committed||s->repair_required;
}
bool pocket_proc_pending(void){return any_pending();}
bool pocket_proc_backdrop_pending(void){return surfaces[0].pending&&!surfaces[0].image_mode;}
bool pocket_proc_is_image_mode(void){return surfaces[0].image_mode;}
void pocket_proc_image_mode_at(unsigned index){
    if(index>=PROC_SURFACES)return;
    surfaces[index].image_mode=true;
    /* A frame may already exist (resource() after beginFrame); the next
     * beginFrame() fails if this does too. */
    if(!image_band_buf)
        image_band_buf=shiki_or_sys_calloc(KSN_PROC_W*PROC_IMAGE_BAND_ROWS,sizeof *image_band_buf);
}
void pocket_proc_image_mode(void){pocket_proc_image_mode_at(0);}
static ksn_result image_span(void *ctx,uint16_t variant,uint16_t frame_number,
                             uint16_t y,uint16_t x,uint16_t count,
                             uint16_t *rgb565,uint8_t *alpha){
    uintptr_t key=(uintptr_t)ctx;
    if(key<1||key>PROC_SURFACES)return KSN_STALE;
    unsigned index=(unsigned)(key-1);
    proc_surface *surface=&surfaces[index];
    if(variant||frame_number||y>=KSN_PROC_H||x>KSN_PROC_W||
       count>KSN_PROC_W-x||(count&&(!rgb565||!alpha)))return KSN_INVALID;
    if(!count)return KSN_OK;
#ifdef KASANE_PROC_JS_DIAGNOSTIC
    uint32_t span_start=esp_cpu_get_cycle_count();
#endif
    const ksn_proc_frame *frame=surface->pending?surface->candidate:
        (surface->has_committed?surface->committed:NULL);
    uint16_t color=surface->pending?surface->candidate_color:
        (surface->has_committed?surface->committed_color:0);
    /* Before the first frame or after reset, a blank image needs no
     * workspace allocation, including when allocations would fail. */
    if(!frame){
        for(unsigned i=0;i<count;i++)rgb565[i]=color;
        memset(alpha,255,count);
        goto span_done;
    }
    uint16_t *image_band=image_band_buf;
    if(!image_band)return KSN_OOM;          /* buffers() takes it for an image surface */
    uint16_t band_y=(uint16_t)(y&~(PROC_IMAGE_BAND_ROWS-1u));
    if(!image_band_valid||image_band_surface!=index||
       image_band_y!=band_y||image_band_frame!=frame||
       image_band_color!=color){
        image_band_valid=false;
        unsigned rows=KSN_PROC_H-band_y;
        if(rows>PROC_IMAGE_BAND_ROWS)rows=PROC_IMAGE_BAND_ROWS;
#if defined(KASANE_PROC_JS_DIAGNOSTIC)||defined(KASANE_MEGADEMO_TRACE)
        uint32_t band_start=esp_cpu_get_cycle_count();
#endif
        for(unsigned i=0;i<rows*KSN_PROC_W;i++)image_band[i]=color;
        if(frame&&!render_rows(surface,frame,image_band,band_y,(int)rows))return KSN_INVALID;
#ifdef KASANE_PROC_JS_DIAGNOSTIC
        image_band_cycles+=esp_cpu_get_cycle_count()-band_start;
        image_band_count++;
#endif
#ifdef KASANE_MEGADEMO_TRACE
        trace.band_cy+=esp_cpu_get_cycle_count()-band_start;
        trace.band_n++;
#endif
        image_band_surface=index;image_band_y=band_y;
        image_band_frame=frame;image_band_color=color;
        image_band_valid=true;
    }
    memcpy(rgb565,image_band+(unsigned)(y-band_y)*KSN_PROC_W+x,count*sizeof *rgb565);
    memset(alpha,255,count);
span_done:
#ifdef KASANE_PROC_JS_DIAGNOSTIC
    image_span_cycles+=esp_cpu_get_cycle_count()-span_start;
    image_span_count++;
#endif
    return KSN_OK;
}
/* read_rows (ksn_ports.h): whole rows written in place, as the backdrop below
 * does -- the background, then the frame's band render over it -- so a band
 * of an unscaled full-width image costs one render and no copies: no band
 * buffer, no 32-pixel spans, no alpha. The same pixels as image_span: the
 * same render of the same frame over the same colour (ksn_proc_render_band
 * enters a line mid-way exactly, test_procedural_band_jump.c). Anything but
 * whole rows is left to the span path. */
static ksn_result image_rows(void *ctx,uint16_t variant,uint16_t frame_number,
                             uint16_t y,uint16_t rows,uint16_t x,uint16_t count,
                             uint16_t *dst,uint16_t stride){
    uintptr_t key=(uintptr_t)ctx;
    if(key<1||key>PROC_SURFACES)return KSN_STALE;
    if(variant||frame_number||!dst||y>KSN_PROC_H||rows>KSN_PROC_H-y)return KSN_INVALID;
    if(x||count!=KSN_PROC_W||stride!=KSN_PROC_W)return KSN_UNSUPPORTED;
    if(!rows)return KSN_OK;
#ifdef KASANE_PROC_JS_DIAGNOSTIC
    uint32_t start=esp_cpu_get_cycle_count();
#endif
    const proc_surface *surface=&surfaces[key-1];
    const ksn_proc_frame *frame=surface->pending?surface->candidate:
        (surface->has_committed?surface->committed:NULL);
    const uint16_t color=surface->pending?surface->candidate_color:
        (surface->has_committed?surface->committed_color:0);
    /* Two pixels a store: the strip's rows start 4-byte aligned (240 px).
     * may_alias: the render below reads and writes these as uint16_t (the
     * strict-aliasing trap of docs/vm/aliasing-types.md). */
    typedef uint32_t pixel_pair __attribute__((may_alias));
    const unsigned n=(unsigned)rows*KSN_PROC_W;
    if(!((uintptr_t)dst&3u)){
        pixel_pair *w=(pixel_pair *)(void *)dst;
        const uint32_t pair=(uint32_t)color|(uint32_t)color<<16;
        for(unsigned i=0;i<n/2;i++)w[i]=pair;
    }else for(unsigned i=0;i<n;i++)dst[i]=color;
    if(frame&&!render_rows(surface,frame,dst,y,rows))return KSN_INVALID;
#ifdef KASANE_PROC_JS_DIAGNOSTIC
    image_band_cycles+=esp_cpu_get_cycle_count()-start;
    image_band_count++;
#endif
    return KSN_OK;
}
void pocket_proc_image_port_at(ksn_image_port *out,unsigned index){
    if(out&&index<PROC_SURFACES)*out=(ksn_image_port){.ctx=(void *)(uintptr_t)(index+1),
                              .width=KSN_PROC_W,.height=KSN_PROC_H,
                              .variants=1,.frames=1,.read_span=image_span,.opaque=true,
                              .read_rows=image_rows};
}
void pocket_proc_image_port(ksn_image_port *out){pocket_proc_image_port_at(out,0);}
ksn_result pocket_proc_backdrop(void *ctx,uint16_t y,uint16_t rows,uint16_t *pixels){
    (void)ctx;
    if(!pixels||y>KSN_PROC_H||rows>KSN_PROC_H-y)return KSN_INVALID;
    const proc_surface *surface=&surfaces[0];
    const ksn_proc_frame *frame=surface->pending?surface->candidate:
        (surface->has_committed?surface->committed:NULL);
    uint16_t color=surface->pending?surface->candidate_color:
        (surface->has_committed?surface->committed_color:0);
    for(unsigned i=0;i<(unsigned)rows*KSN_PROC_W;i++)pixels[i]=color;
    return !frame||render_rows(surface,frame,pixels,y,rows)?KSN_OK:KSN_INVALID;
}
void pocket_proc_present_result(ksn_result result){
    carry_drop();
    proc_surface *surface=&surfaces[pending_surface];
    bool repairing=false;
    for(unsigned i=0;i<PROC_SURFACES;i++)repairing|=surfaces[i].repair_required;
    if(!surface->pending&&!repairing)return;
    if(result==KSN_OK){
        if(surface->pending){
            /* One buffer: the candidate already is the committed frame. */
            surface->committed_color=surface->candidate_color;
            surface->committed_sig=surface->candidate_sig;
            surface->has_committed=true;surface->pending=false;surface->committed_lost=false;
            image_band_valid=false;carry_drop();
        }
        for(unsigned i=0;i<PROC_SURFACES;i++)surfaces[i].repair_required=false;
    }else if(result==KSN_IO){
        image_band_valid=false;carry_drop();
        if(surface->image_mode){
            /* Kasane retries a submitted UI patch against the same image
             * resource. Keep its pixels paired with that ticket until ACK. */
            surface->repair_required=true;
        }else if(surface->committed_lost){
            /* A backdrop has no submitted image node: it repaints the old
             * frame -- but commit() wrote the new one over it, so the new
             * one becomes the committed frame and is repainted instead. */
            surface->committed_color=surface->candidate_color;
            surface->committed_sig=surface->candidate_sig;
            surface->has_committed=true;surface->pending=false;surface->committed_lost=false;
            surface->repair_required=true;
        }else{
            surface->pending=false;
            /* A backdrop has no submitted image node: repaint the old frame
             * (none here: commit() found no committed frame). */
            surface->repair_required=true;
        }
        if(!surface->image_mode)pocket_kasane_invalidate();
    }
}
void pocket_proc_overlay_present_result(ksn_result result){
    if(any_pending()&&surfaces[pending_surface].image_mode)
        pocket_proc_present_result(result);
}
