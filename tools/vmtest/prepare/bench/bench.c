/* Isolated native startup fixture: real adapter/engine, no display or USB I/O.
 * Timing and allocator-accounting are separate passes. Not a DERBY FPS test. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include "quickjs-prepared.h"
#include "bench-input.h"
#ifdef ESP_PLATFORM
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static uint64_t now_ns(void){return (uint64_t)esp_timer_get_time()*1000;}
#else
#include <time.h>
static uint64_t now_ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
#endif
#ifndef BENCH_HEAD
#define BENCH_HEAD "unknown"
#endif
#define REQUIRE(x) do {if(!(x)){fprintf(stderr,"BENCH_FAIL %d: %s\n",__LINE__,#x);abort();}}while(0)
typedef struct {size_t calls,bytes,live,peak,baseline;} meter;
static meter js_meter,native_meter;
static bool accounting;
typedef union {max_align_t alignment;size_t bytes;} allocation_header;
static void *tracked_alloc(meter *m,size_t n){
    if(n>SIZE_MAX-sizeof(allocation_header))return NULL;
    allocation_header *p=malloc(sizeof(*p)+n);if(!p)return NULL;p->bytes=n;
    m->calls++;m->bytes+=n;m->live+=n;if(m->live>m->peak)m->peak=m->live;return p+1;
}
static void tracked_free(meter *m,void *p){if(!p)return;allocation_header *h=(allocation_header *)p-1;REQUIRE(m->live>=h->bytes);m->live-=h->bytes;free(h);}
static void *native_malloc(size_t n){return accounting?tracked_alloc(&native_meter,n):malloc(n);}
static void *native_calloc(size_t c,size_t n){if(!accounting)return calloc(c,n);if(n&&c>SIZE_MAX/n)return NULL;void *p=native_malloc(c*n);if(p)memset(p,0,c*n);return p;}
static void native_free(void *p){if(accounting)tracked_free(&native_meter,p);else free(p);}
#define malloc native_malloc
#define calloc native_calloc
#define free native_free
#include "pocket_proc.c"
#undef malloc
#undef calloc
#undef free
/* Presentation is deliberately outside this registration-only experiment. */
ksn_result pocket_kasane_proc_publish(void){return KSN_OK;}
ksn_result pocket_kasane_proc_publish_at(unsigned s,ksn_rect r){(void)s;(void)r;return KSN_OK;}
void pocket_kasane_invalidate(void){}
JSValue pocket_kasane_proc_resource(JSContext *ctx){(void)ctx;return JS_UNDEFINED;}
JSValue pocket_kasane_proc_resource_at(JSContext *ctx,unsigned n){(void)ctx;(void)n;return JS_UNDEFINED;}
JSValue pocket_api_throw(JSContext *ctx,const char *c,const char *op,const char *msg,bool retry,const char *out){(void)retry;(void)out;return JS_ThrowTypeError(ctx,"%s %s %s",c,op,msg);}
static void *jm(void *o,size_t n){(void)o;return n?tracked_alloc(&js_meter,n):NULL;}
static void *jc(void *o,size_t c,size_t n){if(n&&c>SIZE_MAX/n)return NULL;void *p=jm(o,c*n);if(p)memset(p,0,c*n);return p;}
static void jf(void *o,void *p){(void)o;tracked_free(&js_meter,p);}
static void *jr(void *o,void *p,size_t n){
    if(!n){jf(o,p);return NULL;}if(!p)return jm(o,n);
    allocation_header *h=(allocation_header *)p-1;size_t old=h->bytes;
    if(n>SIZE_MAX-sizeof(*h))return NULL;
    allocation_header *q=realloc(h,sizeof(*h)+n);if(!q)return NULL;q->bytes=n;
    js_meter.calls++;js_meter.bytes+=n;js_meter.live=js_meter.live-old+n;
    if(js_meter.live>js_meter.peak)js_meter.peak=js_meter.live;
    return q+1;
}
static size_t ju(const void *p){return p?((const allocation_header *)p-1)->bytes:0;}
static const JSMallocFunctions mf={jc,jm,jf,jr,ju};
static void reset_window(meter *m){m->calls=m->bytes=0;m->baseline=m->peak=m->live;}
static JSContext *setup(JSRuntime *rt){
    JSContext *ctx=JS_NewContext(rt);REQUIRE(ctx);
    JSValue p=JS_NewObject(ctx),k=JS_NewObject(ctx),g=JS_GetGlobalObject(ctx);
    REQUIRE(pocket_proc_install(ctx,k)==ESP_OK);
    REQUIRE(JS_SetPropertyStr(ctx,p,"kasane",k)>=0);
    REQUIRE(JS_SetPropertyStr(ctx,g,"pocket",p)>=0);JS_FreeValue(ctx,g);
    pocket_proc_rom_plans(ksn_proc_rom_plans,ksn_proc_rom_plans_count);
    /* Fresh native process state, no handles survive between trials. */
    REQUIRE(!prepared_active&&!prepared_consuming);next_handle=0;
    prepared_hits=prepared_misses=0;return ctx;
}
static void trial(unsigned pair,unsigned order,bool fallback,bool prepared,bool metered,bool emit){
    accounting=metered;js_meter=(meter){0};native_meter=(meter){0};
    JSRuntime *rt=metered?JS_NewRuntime2(&mf,NULL):JS_NewRuntime();REQUIRE(rt);
    JSContext *ctx=setup(rt);const char *source=fallback?bench_fallback:bench_fixture;
    reset_window(&js_meter);reset_window(&native_meter);
    size_t free0=0,minfree=0,largest=0;
#ifdef ESP_PLATFORM
    unsigned caps=MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT;
    REQUIRE(heap_caps_monitor_local_minimum_free_size_start()==ESP_OK);free0=heap_caps_get_free_size(caps);
#endif
    uint64_t start=now_ns();
    JSValue v=prepared?pocket_proc_eval_prepared(ctx,source,strlen(source),"runtime-startup.js"):
                       JS_Eval(ctx,source,strlen(source),"runtime-startup.js",JS_EVAL_TYPE_GLOBAL);
    uint64_t elapsed=now_ns()-start;
#ifdef ESP_PLATFORM
    minfree=heap_caps_get_minimum_free_size(caps);largest=heap_caps_get_largest_free_block(caps);
    REQUIRE(heap_caps_monitor_local_minimum_free_size_stop()==ESP_OK);
#endif
    /* All validation and printing happen after the timed/heap interval. */
    REQUIRE(!JS_IsException(v));int32_t handle=0;REQUIRE(!JS_ToInt32(ctx,&handle,v)&&handle==1);
    REQUIRE(slots[0].rom&&slots[0].plan&&((ksn_proc_rom_plan *)slots[0].plan)->params==1);
    REQUIRE(((ksn_proc_rom_plan *)slots[0].plan)->args[0]==512.0f);
    REQUIRE(prepared_hits==(unsigned)(prepared&&!fallback)&&prepared_misses==0);
    REQUIRE(!prepared_active&&!prepared_consuming);
    meter j=js_meter,n=native_meter;
    if(emit)printf("{\"pair\":%u,\"order\":%u,\"case\":\"%s\",\"mode\":\"%s\",\"pass\":\"%s\",\"ns\":%llu,\"hit\":%u,\"handle\":%ld,\"arg\":512,\"js_calls\":%zu,\"js_bytes\":%zu,\"js_peak_delta\":%zu,\"js_live_delta\":%td,\"js_baseline\":%zu,\"js_peak_live\":%zu,\"native_calls\":%zu,\"native_bytes\":%zu,\"native_peak_delta\":%zu,\"native_live_delta\":%td,\"native_baseline\":%zu,\"native_peak_live\":%zu,\"internal_free_before\":%zu,\"internal_min_free\":%zu,\"internal_largest_after\":%zu}\n",pair,order,fallback?"fallback":"supported",prepared?"prepared":"generic",metered?"accounting":"timing",(unsigned long long)elapsed,prepared_hits,(long)handle,j.calls,j.bytes,j.peak-j.baseline,(ptrdiff_t)j.live-(ptrdiff_t)j.baseline,j.baseline,j.peak,n.calls,n.bytes,n.peak-n.baseline,(ptrdiff_t)n.live-(ptrdiff_t)n.baseline,n.baseline,n.peak,free0,minfree,largest);
    JS_FreeValue(ctx,v);pocket_proc_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);
    REQUIRE(!metered||(!js_meter.live&&!native_meter.live));
#ifdef ESP_PLATFORM
    vTaskDelay(1); /* watchdog/yield outside measurement */
#endif
}
static void run_bench(unsigned pairs){
    printf("{\"schema\":\"prepared-bench-v1\",\"head\":\"%s\",\"pointer_bytes\":%zu,\"pairs\":%u,\"warmup_pairs\":8,\"condition\":\"fresh runtime/context/native state; warmed process/cache; setup/teardown excluded\",\"bytes\":\"requested payload; realloc counts full new request; accounting headers excluded\"}\n",BENCH_HEAD,sizeof(void *),pairs);
    for(unsigned pass=0;pass<2;pass++)for(unsigned c=0;c<2;c++){
        for(unsigned i=0;i<8;i++)for(unsigned o=0;o<2;o++)trial(i,o,c,(i+o)&1,pass,false);
        for(unsigned i=0;i<pairs;i++)for(unsigned o=0;o<2;o++)trial(i,o,c,(i+o)&1,pass,true);
    }
    puts("{\"complete\":true}");
}
#ifdef ESP_PLATFORM
void app_main(void){run_bench(64);puts("PREPARED_BENCH_DONE no display/USB commands issued");}
#else
int main(int argc,char **argv){unsigned n=argc>1?(unsigned)strtoul(argv[1],NULL,10):200;REQUIRE(n>=10&&n<=10000);run_bench(n);return 0;}
#endif
