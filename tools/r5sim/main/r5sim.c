// STRESS's turns on the chip, for QEMU (docs/vm/turn-icache.md sec.1). The setup is
// tools/test_stress_app.c's -- real QuickJS, real pocket.kasane, a pocket.fs
// stub -- but frame() is called as app_tick calls it (JS_Call, then the job
// queue drained, then pocket_kasane_end_turn), not through JS_Eval, so the
// parser does not run inside a turn the way it does on the host.
//
// The turns are marked for the trace by calling r5_turn_begin/r5_turn_end:
// QEMU's exec log names a translation block by its address, and these two
// are the only code at theirs, so tools/r5sim/icache.py cuts the fetch stream
// at them without any clock or output having to line up.
#include "esp_heap_caps.h"
#include "pocket_kasane.h"
#include "pocket_memory.h"
#include "pocket_av.h"
#include "system/sys_device.h"
#include "ui/kasane/ksn_runtime.h"
#include "ui/kasane/ksn_render.h"
#include "text/ksn_font.h"
#include "sound.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void host_capabilities_clear(void);
int32_t pocket_av_ui_current_player(void){return 0;}
bool pocket_av_ui_read(int32_t id,pocket_av_ui_snapshot *out){(void)id;(void)out;return false;}
bool sys_device_clock_read(sys_clock_state *out){(void)out;return false;}
void sound_stream_set_observer(sound_stream_observer_fn o){(void)o;}
void sound_stream_set_observer_interval(sound_stream_observer_fn o,uint32_t n){(void)o;(void)n;}
// pocket_kasane.c now installs and polls the proc/grid/video/pixel surfaces.
// STRESS uses none of them, and their real sources pull in the DERBY runner
// and PIE kernels, so they are stubbed as tools/test_keytest_app.c does. On
// the device each per-turn call is a flag test of a few instructions.
#include "pocket_shiki_frame.h"
esp_err_t pocket_proc_install(JSContext *c,JSValueConst k){(void)c;(void)k;return ESP_OK;}
#ifndef R5SIM_APP_SHIKI   // pocket_shiki_frame.c has the real ones
void pocket_proc_end_turn(void){}
bool pocket_proc_has_frame(void){return false;}
bool pocket_proc_pending(void){return false;}
bool pocket_proc_backdrop_pending(void){return false;}
bool pocket_proc_is_image_mode(void){return false;}
ksn_result pocket_proc_backdrop(void *o,uint16_t y,uint16_t rows,uint16_t *px){(void)o;(void)y;(void)rows;(void)px;return KSN_OK;}
void pocket_proc_present_result(ksn_result r){(void)r;}
#endif
esp_err_t pocket_grid_install(JSContext *c,JSValueConst k){(void)c;(void)k;return ESP_OK;}
void pocket_grid_reset(void){}
bool pocket_grid_pending(void){return false;}
void pocket_grid_present_result(ksn_result r){(void)r;}
void pocket_grid_source_invalidated(uint32_t id){(void)id;}
esp_err_t pocket_video_install(JSContext *c,JSValueConst k){(void)c;(void)k;return ESP_OK;}
void pocket_video_reset(void){}
bool pocket_video_pending(void){return false;}
void pocket_video_present_result(ksn_result r){(void)r;}
esp_err_t pocket_pixel_install(JSContext *c,JSValueConst k){(void)c;(void)k;return ESP_OK;}
void pocket_pixel_reset(void){}
bool pocket_pixel_pending(void){return false;}
void pocket_pixel_present_result(ksn_result r){(void)r;}

#ifdef R5SIM_GUEST_ALLOC
// The firmware guest's allocator (components/pocketjs_guest/src/guest.c,
// -DR5SIM_GUEST_ALLOC=1): the R3 small-block cache over heap_caps, same
// classes, block length read from the tlsf header. Without it the image runs
// JS_NewRuntime's malloc -> tlsf + heap lock on every allocation, which was
// 44% of a turn's instructions (docs/vm/qjs-observe.md sec.2) -- a cost the
// device does not pay on a cache hit.
#include "esp_heap_caps.h"
#include "pocketjs/block_cache.h"
#define R5_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
static block_cache_t r5_cache;
static size_t r5_len(const void *p){ return *(const uint32_t *)((const char *)p-4)&~(uint32_t)3U; }
static void *r5_bm(void *c,size_t n){(void)c;return heap_caps_malloc(n,R5_CAPS);}
static void r5_bf(void *c,void *b){(void)c;heap_caps_free(b);}
static size_t r5_bl(void *c,const void *b){(void)c;return r5_len(b);}
static const block_cache_backend_t R5_BACKEND={r5_bm,r5_bf,r5_bl,NULL};
static inline size_t r5_round(size_t n){ size_t a=(n+3U)&~(size_t)3U; return a<12U?12U:a; }
// Which sizes miss the cache, and which frees it cannot keep, in the measured
// turns (docs/vm/qjs-observe.md sec.10): index = rounded length / 4, the last
// slot everything above BLOCK_CACHE_MAX_SIZE.
static bool r5_hist_on;
static size_t r5_held_max;
static uint32_t r5_miss_hist[BLOCK_CACHE_MAX_SIZE/4+2], r5_spill_hist[BLOCK_CACHE_MAX_SIZE/4+2], r5_alloc_hist[BLOCK_CACHE_MAX_SIZE/4+2];
static inline unsigned r5_slot(size_t n){ return n>BLOCK_CACHE_MAX_SIZE?BLOCK_CACHE_MAX_SIZE/4+1:(unsigned)(n/4); }
static void *r5_malloc(void *o,size_t n){
    (void)o;
    if(!n) return NULL;
    const uint32_t m=r5_cache.misses;
    void *p=block_cache_malloc(&r5_cache,n,r5_round(n));
    if(r5_hist_on&&r5_cache.misses!=m) r5_miss_hist[r5_slot(r5_round(n))]++;
    if(r5_hist_on) r5_alloc_hist[r5_slot(r5_round(n))]++;
    return p;
}
static void *r5_calloc(void *o,size_t k,size_t n){ if(k&&n>SIZE_MAX/k)return NULL; void *p=r5_malloc(o,k*n); if(p)memset(p,0,k*n); return p; }
static void r5_free(void *o,void *p){
    (void)o;
    const uint32_t q=r5_cache.pushes;
    const size_t n=p?r5_len(p):0;
    block_cache_free(&r5_cache,p);
    if(r5_hist_on&&p&&r5_cache.pushes==q) r5_spill_hist[r5_slot(n)]++;
}
static void r5_hist_print(const char *what,const uint32_t *h){
    uint32_t total=0; for(unsigned i=0;i<BLOCK_CACHE_MAX_SIZE/4+2;i++) total+=h[i];
    printf("R5SIM %s total=%u:",what,(unsigned)total);
    for(unsigned k=0;k<16;k++){
        unsigned best=0; for(unsigned i=1;i<BLOCK_CACHE_MAX_SIZE/4+2;i++) if(h[i]>h[best]) best=i;
        if(!h[best]) break;
        if(best==BLOCK_CACHE_MAX_SIZE/4+1) printf(" >%u:%u",(unsigned)BLOCK_CACHE_MAX_SIZE,(unsigned)h[best]);
        else printf(" %u:%u",best*4,(unsigned)h[best]);
        ((uint32_t *)h)[best]=0;
    }
    printf("\n");
}
static void *r5_realloc(void *o,void *p,size_t n){
    if(!p)return r5_malloc(o,n);
    if(!n){r5_free(o,p);return NULL;}
    return heap_caps_realloc(p,n,R5_CAPS);
}
static size_t r5_usable(const void *p){return p?r5_len(p):0;}
static const JSMallocFunctions R5_ALLOC={r5_calloc,r5_malloc,r5_free,r5_realloc,r5_usable};
#endif

#ifdef R5SIM_APP_SHIKI
#include "shiki_heap.h"
extern const char stress_js_start[] asm("_binary_stress_js_start");
extern const char stress_js_end[] asm("_binary_stress_js_end");
#define R5SIM_APP_NAME "stress.js"
#else
extern const char stress_js_start[] asm("_binary_stress_r5_js_start");
extern const char stress_js_end[] asm("_binary_stress_r5_js_end");
#define R5SIM_APP_NAME "stress_r5.js"
#endif

#ifndef R5SIM_FRAMES
#define R5SIM_FRAMES 120
#endif

// Distinct bodies so the linker cannot fold them into one address.
volatile unsigned r5_turn_now, r5_turn_done;
__attribute__((noinline,used)) void r5_turn_begin(unsigned t){ r5_turn_now=t; }
__attribute__((noinline,used)) void r5_turn_end(unsigned t){ r5_turn_done=t+1; }
// The render's own marks (observe.py --begin r5_render_begin --end r5_render_end).
volatile unsigned r5_render_now;
__attribute__((noinline,used)) void r5_render_begin(unsigned t){ r5_render_now=t; }
__attribute__((noinline,used)) void r5_render_end(unsigned t){ r5_render_now=t+1000; }

static JSRuntime *rt;
static JSContext *ctx;
#ifdef R5SIM_DEVICE
// -DR5SIM_DEVICE=1: the session as the firmware builds it, through the same
// component -- pocketjs_guest_create (allocator with the R3 block cache,
// memory limit, GC threshold, stack limit, hardened context, interrupt
// handler), pocketjs_guest_eval, and per turn arm_turn()'s watchdog deadline
// and job budget around pocketjs_guest_frame (main/app_session.c). With the
// firmware's helper-IRAM fragment linked (tools/r5sim/main/CMakeLists.txt).
#include "pocketjs/guest.h"
#include "pocketjs/guest_quickjs.h"
#include "pocketjs/vm_sched.h"
#include "esp_timer.h"
static pocketjs_guest_t *guest;
static int64_t r5_deadline;
static int r5_watchdog(void *o){(void)o;return esp_timer_get_time()>r5_deadline;}  // app_session.c interrupt()
static vm_budget_t r5_budget;
static void r5_arm_turn(void){                                                     // app_session.c arm_turn()
    r5_deadline=esp_timer_get_time()+250000;
    vm_budget_begin(&r5_budget,VM_TURN_BUDGET_US);
    pocketjs_guest_budget(guest,&r5_budget);
    pocketjs_guest_yield_enabled(guest,true);
}
#endif
#ifdef R5SIM_GC_TRACE
bool pocket_proc_pending(void);
void JS_PocketGCStats(uint32_t *runs, uint64_t *freed, size_t *last_before, size_t *last_after);
void JS_PocketGCTime(uint64_t *total, uint32_t *max_run);
#include "esp_cpu.h"
uint32_t pocket_gc_clock(void) { return esp_cpu_get_cycle_count(); }
#endif
static uint16_t strip_pixels[240*8];
static unsigned oom_lines, fail_lines;
#include "esp_cpu.h"
#define R5_CC_TAKE() do { if(t>60){ uint32_t d=esp_cpu_get_cycle_count()-cc0; r5_cc+=d; if(d>r5_cc_max)r5_cc_max=d; } } while(0)

static JSValue js_log(JSContext *c,JSValueConst self,int argc,JSValueConst *argv) {
    (void)self;
    const char *s=argc?JS_ToCString(c,argv[0]):NULL;
    if(!s) return JS_UNDEFINED;
    if(!strncmp(s,"STRESS_OOM",10)) oom_lines++;
    else if(!strncmp(s,"STRESS_FAIL",11)) { fail_lines++; printf("R5SIM %s\n",s); }
    else if(!strncmp(s,"STRESS f=",9)) printf("R5SIM %s\n",s);
    JS_FreeCString(c,s);
    return JS_UNDEFINED;
}
static JSValue js_nop(JSContext *c,JSValueConst self,int argc,JSValueConst *argv) {
    (void)c;(void)self;(void)argc;(void)argv; return JS_UNDEFINED;
}

static void drain(void) {
    JSContext *c;
    while(JS_ExecutePendingJob(rt,&c)>0) {}
    pocket_kasane_end_turn();
}
#ifdef R5SIM_MEMUSAGE
static void memusage(const char *name) {
    /* What the guest holds after evaluating `name`, before and after a GC:
     * which of it the app needs to run, and which a host could purge. */
    for(int pass=0;pass<2;pass++) {
        if(pass) JS_RunGC(rt);
        JSMemoryUsage u; JS_ComputeMemoryUsage(rt,&u);
        printf("R5SIM MEMUSAGE %s gc=%d malloc=%lld/%lld used=%lld/%lld atoms=%lld/%lld str=%lld/%lld "
               "obj=%lld/%lld prop=%lld/%lld shape=%lld/%lld jsfunc=%lld/%lld code=%lld pc2line=%lld/%lld "
               "cfunc=%lld arrays=%lld fast=%lld/%lld bin=%lld/%lld\n",name,pass,
               (long long)u.malloc_count,(long long)u.malloc_size,(long long)u.memory_used_count,
               (long long)u.memory_used_size,(long long)u.atom_count,(long long)u.atom_size,
               (long long)u.str_count,(long long)u.str_size,(long long)u.obj_count,(long long)u.obj_size,
               (long long)u.prop_count,(long long)u.prop_size,(long long)u.shape_count,(long long)u.shape_size,
               (long long)u.js_func_count,(long long)u.js_func_size,(long long)u.js_func_code_size,
               (long long)u.js_func_pc2line_count,(long long)u.js_func_pc2line_size,(long long)u.c_func_count,
               (long long)u.array_count,(long long)u.fast_array_count,(long long)u.fast_array_elements,
               (long long)u.binary_object_count,(long long)u.binary_object_size);
    }
}
#endif
static bool eval(const char *source,size_t len,const char *name) {
    JSValue v=JS_Eval(ctx,source,len,name,JS_EVAL_TYPE_GLOBAL);
    bool ok=!JS_IsException(v);
    if(!ok) {
        JSValue e=JS_GetException(ctx);
        const char *t=JS_ToCString(ctx,e);
        printf("R5SIM %s threw: %s\n",name,t?t:"?");
        if(t) JS_FreeCString(ctx,t);
        JS_FreeValue(ctx,e);
    }
    JS_FreeValue(ctx,v);
    drain();
#ifdef R5SIM_MEMUSAGE
    memusage(name);
#endif
    return ok;
}

static uint16_t *get_strip(void *o) { (void)o; return strip_pixels; }
static ksn_result send_strip(void *o,uint16_t y,uint16_t rows,const uint16_t *pixels) {
    (void)o;(void)y;(void)rows;(void)pixels; return KSN_OK;
}
static bool present(void) {
    if(!pocket_kasane_needs_present()) return true;
    ksn_display_port port={.strip=get_strip,.present=send_strip,
                           .width=240,.height=135,.strip_rows=8,.text=&ksn_font_port};
    ksn_render_stats stats;
    ksn_result r=pocket_kasane_present(&port,&stats);
#ifdef R5SIM_GC_TRACE
    if(r!=KSN_OK)
        printf("R5SIM PRESENT_FAIL r=%u sys_free=%u sys_largest=%u\n",(unsigned)r,
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
#endif
    return r==KSN_OK;
}

// tools/test_stress_app.c's pocket.fs stub, without its fault-injection hooks.
static const char FS_STUB[]=
    "globalThis.console={log:globalThis.__log};"
    "globalThis.pocket={kasane:globalThis.kasane,fs:{open:async function(){let n=0;"
    "return{read:async function(m){__readStarted();let c=++n>3?null:new Uint8Array(m);"
    "__readFinished();return c},close:function(){}}}},memory:globalThis.memory};";

#ifdef R5SIM_PGO_GEN
// The PGO training dump (docs/vm/qjs-pgo.md): libgcov's freestanding interface
// encodes each gcov_info record as a gcda stream, printed in hex lines that
// tools/pgo/train.sh turns back into bytes for `gcov-tool merge-stream`.
struct gcov_info;
extern const struct gcov_info *const _gcov_info_start[], *const _gcov_info_end[];
void __gcov_info_to_gcda(const struct gcov_info *, void (*)(const char *, void *),
                         void (*)(const void *, unsigned, void *),
                         void *(*)(unsigned, void *), void *);
void __gcov_filename_to_gcfn(const char *, void (*)(const void *, unsigned, void *), void *);
static void gcov_hex(const void *d, unsigned n, void *arg) {
    (void)arg;
    const uint8_t *p = d;
    while (n) {
        unsigned k = n < 48 ? n : 48;
        printf("GCOV ");
        for (unsigned i = 0; i < k; i++) printf("%02x", p[i]);
        printf("\n");
        p += k; n -= k;
    }
}
static void gcov_name(const char *f, void *arg) { __gcov_filename_to_gcfn(f, gcov_hex, arg); }
static void *gcov_alloc(unsigned n, void *arg) { (void)arg; return malloc(n); }
static void gcov_dump(void) {
    printf("GCOV begin\n");
    for (const struct gcov_info *const *i = _gcov_info_start; i < _gcov_info_end; i++)
        __gcov_info_to_gcda(*i, gcov_name, gcov_hex, gcov_alloc, NULL);
    printf("GCOV end\n");
}
#endif

void app_main(void) {
#ifdef R5SIM_APP_SHIKI
    // The manifest names kasane.shiki: the session takes the Shiki heap before
    // the guest (app_session.c), and pocket.kasane then has shiki.
    if(!shiki_heap_begin(0)){ printf("R5SIM shiki heap FAIL\n"); return; }
#endif
#if defined(R5SIM_DEVICE)
    {
        pocketjs_guest_config_t gc;
        pocketjs_guest_config_defaults(&gc);
        gc.heap_limit=160*1024; gc.stack_limit=20*1024; gc.prefer_psram=false;  // app_session.c
        if(pocketjs_guest_create(&gc,&guest)!=ESP_OK){ printf("R5SIM guest_create FAIL\n"); return; }
        pocketjs_guest_set_watchdog(guest,r5_watchdog,NULL);
        r5_deadline=esp_timer_get_time()+2000000;   // app_session.c: 2 s for the evaluation
        ctx=pocketjs_guest_quickjs_context(guest);
        rt=JS_GetRuntime(ctx);
        printf("R5SIM device session\n");
    }
#elif defined(R5SIM_GUEST_ALLOC)
    {
#ifndef R5SIM_CLASSES
#define R5SIM_CLASSES 12,16,24,28,32,36,48,80   /* guest.c's */
#endif
        static const uint16_t classes[]={R5SIM_CLASSES};  // guest.c's unless overridden
        void *probe=heap_caps_malloc(20,R5_CAPS);
        bool direct=probe&&r5_len(probe)==heap_caps_get_allocated_size(probe);
        heap_caps_free(probe);
        printf("R5SIM guest_alloc direct=%d\n",(int)direct);
        block_cache_init(&r5_cache,&R5_BACKEND,classes,sizeof(classes)/sizeof(classes[0]),direct);
    }
    rt=JS_NewRuntime2(&R5_ALLOC,NULL);
#else
    rt=JS_NewRuntime();
#endif
#ifndef R5SIM_DEVICE
    ctx=JS_NewContext(rt);
#endif
    host_capabilities_clear();
    pocket_kasane_install(ctx,NULL);
    // Rendering only has to consume each submission; a 16x8 viewport keeps the
    // scalar renderer from filling the trace with pixels.
    pocket_kasane_set_viewport(0,0,16,8);
    pocket_memory_install(ctx,NULL);
    JSValue g=JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx,g,"__log",JS_NewCFunction(ctx,js_log,"log",1));
    JS_SetPropertyStr(ctx,g,"__readStarted",JS_NewCFunction(ctx,js_nop,"readStarted",0));
    JS_SetPropertyStr(ctx,g,"__readFinished",JS_NewCFunction(ctx,js_nop,"readFinished",0));
    eval(FS_STUB,strlen(FS_STUB),"stub.js");
#ifdef R5SIM_DEVICE
    // pocketjs_guest_eval binds globalThis.frame and drains, as app_start_test.
    bool ok=pocketjs_guest_eval(guest,stress_js_start,(size_t)(stress_js_end-stress_js_start-1),
                                R5SIM_APP_NAME)==ESP_OK;
#ifdef R5SIM_MEMUSAGE
    memusage(R5SIM_APP_NAME);
#endif
    pocket_kasane_end_turn();
    ok=ok&&present();
    unsigned conts=0,frame_err=0;
    pocketjs_guest_frame_t input={.struct_size=sizeof(input),.buttons=0,.analog=0x8080};
#else
    bool ok=eval(stress_js_start,(size_t)(stress_js_end-stress_js_start-1),R5SIM_APP_NAME)&&present();
    // The device's arrangement (main/app_session.c, pocketjs_guest_create).
    JS_SetMemoryLimit(rt,160*1024);
    JS_SetGCThreshold(rt,80*1024);
    JSValue frame=JS_GetPropertyStr(ctx,g,"frame");
#endif
    printf("R5SIM eval %s\n",ok?"ok":"FAIL");
#ifdef R5SIM_FREE_KIB
    {
        /* The device's free internal heap when STRESS starts (MEM free=~91 KB
         * after the evaluation, measured): QEMU has no radio and no panel. */
        const uint32_t caps=MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT;
        const size_t target=(size_t)R5SIM_FREE_KIB*1024u;
        unsigned chunks=0;
        while(heap_caps_get_free_size(caps)>target+8192u&&heap_caps_malloc(4096,caps)) chunks++;
        size_t f=heap_caps_get_free_size(caps);
        if(f>target+64u&&heap_caps_malloc(f-target-64u,caps)) chunks++;
        printf("R5SIM ballast chunks=%u free=%u largest=%u\n",chunks,
               (unsigned)heap_caps_get_free_size(caps),(unsigned)heap_caps_get_largest_free_block(caps));
    }
#endif
#ifdef R5SIM_HEAP_INFO
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
#endif
    unsigned bad_present=0;
    // The fast measure (docs/vm/qjs-observe.md sec.9): under -icount QEMU's
    // CCOUNT advances in proportion to executed instructions, so the turns'
    // CCOUNT is their instruction count up to one constant -- read without
    // the -d trace (seconds instead of half an hour). Turns 61..120 as the
    // trace records use.
    uint64_t r5_cc=0, r5_cc_max=0, r5_cc_end=0;   // r5_cc_end: pocket_kasane_end_turn's share
    for(unsigned t=1;ok&&t<=R5SIM_FRAMES;t++) {
        const uint32_t cc0=esp_cpu_get_cycle_count();
#ifdef R5SIM_DEVICE
#ifdef R5SIM_LEVEL_UP
        // STRESS L1 -> L2 -> L3 as the host test does at 300/600: here at
        // 40/80, so the run reaches L3's deliberate OOM and its recovery.
        input.buttons=(t==40||t==80)?0x4000u:0u;
#endif
        r5_arm_turn();
        r5_turn_begin(t);
        if(pocketjs_guest_frame(guest,&input)!=ESP_OK) frame_err++;
        // A frame() cut by the 8 ms budget finishes in continuation turns on
        // the device's next ticks; here they stay inside this turn's marks so
        // a turn is a frame's whole JS. Counted, and normally 0 under QEMU.
        while(pocketjs_guest_work_pending(guest)) { conts++; r5_arm_turn(); pocketjs_guest_continue(guest); }
        const uint32_t cc_js=esp_cpu_get_cycle_count();
        pocket_kasane_end_turn();
        if(t>60) r5_cc_end+=esp_cpu_get_cycle_count()-cc_js;
        r5_turn_end(t);
        R5_CC_TAKE();
#else
        JSValue b=JS_NewInt32(ctx,0);
#ifdef R5SIM_GUEST_ALLOC
        r5_hist_on=t>60;
        if(r5_hist_on&&block_cache_held(&r5_cache)>r5_held_max) r5_held_max=block_cache_held(&r5_cache);
#endif
        r5_turn_begin(t);
        JSValue r=JS_Call(ctx,frame,JS_UNDEFINED,1,&b);
        if(JS_IsException(r)) JS_FreeValue(ctx,JS_GetException(ctx));
        JS_FreeValue(ctx,r);
        drain();
        r5_turn_end(t);
        R5_CC_TAKE();
#endif
#if defined(R5SIM_FIT_LIMIT_KIB) && defined(R5SIM_DEVICE)
        pocketjs_guest_fit_limit(guest,heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
                                 (size_t)R5SIM_FIT_LIMIT_KIB*1024u);
#endif
        JSOOMCanary canary={0};
        JS_TakeOOMCanary(rt,&canary);
        if(canary.count) pocket_memory_oom(&canary,(uint64_t)t*33333u);
        size_t used=0,limit=0;
        JS_GetMemoryCounters(rt,&used,&limit);
#ifdef R5SIM_GC_TRACE
        {
            static uint32_t runs_seen; static uint64_t freed_seen;
            uint32_t runs; uint64_t freed; size_t lb,la;
            JS_PocketGCStats(&runs,&freed,&lb,&la);
            static uint64_t ticks_seen; uint64_t ticks; uint32_t tmax;
            JS_PocketGCTime(&ticks,&tmax);
            unsigned ck_n; size_t ck_b; uint32_t ck_t,ck_r;
            pocketjs_guest_chunk_stats(guest,&ck_n,&ck_b,&ck_t,&ck_r);
#ifdef R5SIM_CHUNK_TRACE
            { void pocketjs_guest_chunk_trace(pocketjs_guest_t *, unsigned);
              if(canary.count||t%50==0) pocketjs_guest_chunk_trace(guest,t); }
#endif
            const uint32_t caps=MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT;
            printf("R5SIM GC t=%u used=%u lim=%u thr=%u gc=%u(+%u) freed=+%u last=%u->%u "
                   "oom=%u q=%u a=%u req=%u sys_free=%u sys_largest=%u pend=%d needs=%d sub=%d "
                   "gc_cc=+%u gc_cc_max=%u chunks=%u/%u taken=%u ret=%u\n",
                   t,(unsigned)used,(unsigned)limit,(unsigned)JS_GetGCThreshold(rt),(unsigned)runs,
                   (unsigned)(runs-runs_seen),(unsigned)(freed-freed_seen),(unsigned)lb,(unsigned)la,
                   (unsigned)canary.count,(unsigned)canary.quota_count,(unsigned)canary.allocator_count,
                   (unsigned)canary.first_req,(unsigned)heap_caps_get_free_size(caps),
                   (unsigned)heap_caps_get_largest_free_block(caps),
                   (int)pocket_proc_pending(),(int)pocket_kasane_needs_present(),
                   (int)pocket_kasane_has_submission(),(unsigned)(ticks-ticks_seen),(unsigned)tmax,
                   ck_n,(unsigned)ck_b,(unsigned)ck_t,(unsigned)ck_r);
            runs_seen=runs; freed_seen=freed; ticks_seen=ticks;
        }
#endif
#ifdef R5SIM_GC_ON_OOM
        /* The experiment: the system heap refused this turn; collect now
         * (between turns, where a GC is safe) and report what it freed. */
        if(canary.allocator_count) {
            size_t u0=used;
            JS_RunGC(rt);
            JS_GetMemoryCounters(rt,&used,&limit);
            printf("R5SIM GC_ON_OOM t=%u used=%u->%u\n",t,(unsigned)u0,(unsigned)used);
        }
#endif
        pocket_memory_sample((uint64_t)t*33333u,used,limit,false,0,0);
        pocket_memory_pump(false);
#ifdef R5SIM_FULL_RENDER
        // The whole panel from frame 59, to observe the render itself
        // (docs/vm/qjs-observe.md sec.21) without paying for it in the trace
        // of the first 58 frames.
        if(t==59) pocket_kasane_set_viewport(0,0,240,135);
#endif
        r5_render_begin(t);
        if(!present()) bad_present++;
        r5_render_end(t);
    }
#ifdef R5SIM_DEVICE
    pocketjs_guest_stats_t st={0};
    st.struct_size=sizeof(st);
    pocketjs_guest_stats(guest,&st);
    printf("R5SIM device frames=%u frame_errors=%u yields=%u continuations=%u(%u) heap_used=%u\n",
           (unsigned)st.frames,(unsigned)st.frame_errors,(unsigned)st.yields,
           (unsigned)st.continuations,conts,(unsigned)st.heap_used);
#else
    JS_FreeValue(ctx,frame);
#endif
    JS_FreeValue(ctx,g);
#ifdef R5SIM_PGO_GEN
    gcov_dump();
#endif
#ifdef R5SIM_GUEST_ALLOC
    r5_hist_print("alloc_by_size",r5_alloc_hist);
    r5_hist_print("miss_by_size",r5_miss_hist);
    r5_hist_print("spill_by_size",r5_spill_hist);
    printf("R5SIM block_cache held_max=%u\n",(unsigned)r5_held_max);
    printf("R5SIM block_cache hits=%u misses=%u pushes=%u flushes=%u\n",(unsigned)r5_cache.hits,
           (unsigned)r5_cache.misses,(unsigned)r5_cache.pushes,(unsigned)r5_cache.flushes);
#endif
    printf("R5SIM ccount turns=61..%u per_turn=%llu max=%llu end_turn=%llu\n",(unsigned)R5SIM_FRAMES,
           (unsigned long long)(r5_cc/(R5SIM_FRAMES>60?R5SIM_FRAMES-60:1)),(unsigned long long)r5_cc_max,
           (unsigned long long)(r5_cc_end/(R5SIM_FRAMES>60?R5SIM_FRAMES-60:1)));
    printf("R5SIM done frames=%u oom=%u fail=%u bad_present=%u\n",
           (unsigned)R5SIM_FRAMES,oom_lines,fail_lines,bad_present);
}
