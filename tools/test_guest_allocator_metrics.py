#!/usr/bin/env python3
"""Exercise production guest allocator counters with host and fake-device heaps.

The selected production allocator bodies are compiled unchanged. The device
backend models four-byte usable-size rounding and owner identity, not ESP-IDF
timing/fragmentation. Real block_cache code is used. No shared-tree mutation.
"""
from pathlib import Path
import argparse
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
GUEST = ROOT / 'components/pocketjs_guest/src/guest.c'

C = r'''
#undef NDEBUG
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pocketjs/guest.h"
#include "pocketjs/block_cache.h"
#ifndef ESP_ERR_NOT_SUPPORTED
#define ESP_ERR_NOT_SUPPORTED 0x106
#endif
#if TEST_DEVICE
#define ESP_PLATFORM 1
#endif
#define GUEST_CAPS_INTERNAL 1
#define GUEST_CAPS_PSRAM 2
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_SPIRAM 2
#define MALLOC_CAP_8BIT 4
static size_t backend_live;
static bool reject_allocations;
typedef union { size_t n; max_align_t alignment; } backend_header;
static void *heap_caps_malloc(size_t n,unsigned caps) {
    (void)caps;if(reject_allocations||n>4096)return NULL;
#if TEST_DEVICE
    n=(n+3u)&~(size_t)3u;if(n<12)n=12;
#endif
    backend_header *h=malloc(sizeof(*h)+n);assert(h);h->n=n;backend_live+=n;return h+1;
}
static void heap_caps_free(void *p) {
    if(p){backend_header *h=(backend_header *)p-1;backend_live-=h->n;free(h);}
}
#if TEST_DEVICE
static size_t guest_block_length(const void *p) {return p?((const backend_header *)p-1)->n:0;}
static void *heap_caps_realloc(void *p,size_t n,unsigned caps) {
    void *q=heap_caps_malloc(n,caps);if(!q)return NULL;
    if(p){size_t old=guest_block_length(p);memcpy(q,p,old<n?old:n);heap_caps_free(p);}return q;
}
static void *heap_caps_realloc_prefer(void *p,size_t n,int count,unsigned a,unsigned b) {
    (void)count;(void)b;return heap_caps_realloc(p,n,a);
}
static int task=1;
static int xTaskGetCurrentTaskHandle(void){return task;}
#endif
typedef union {size_t size;max_align_t alignment;} allocation_header_t;
struct pocketjs_guest {
    bool prefer_psram;
#if POCKET_GUEST_ALLOCATOR_METRICS
    pocketjs_guest_allocator_stats_t allocator_stats;
#endif
    block_cache_t block_cache;
    int owner_task;
    bool block_cache_on;
};
@RAW@
@COUNTERS@
#if TEST_DEVICE
static void *cache_malloc(void *ctx,size_t n){(void)ctx;return heap_caps_malloc(n,1);}
static void cache_free(void *ctx,void *p){(void)ctx;heap_caps_free(p);}
static size_t cache_length(void *ctx,const void *p){(void)ctx;return guest_block_length(p);}
static const block_cache_backend_t backend={cache_malloc,cache_free,cache_length,NULL};
#endif
int main(void){
    pocketjs_guest_t g={0};
#if TEST_DEVICE
    g.owner_task=1;const uint16_t sizes[]={16,32,48};
    block_cache_init(&g.block_cache,&backend,sizes,3,true);g.block_cache_on=true;
#endif
#if POCKET_GUEST_ALLOCATOR_METRICS
    assert(guest_malloc(&g,0)==NULL);guest_free(&g,NULL);
    assert(g.allocator_stats.allocation_calls==0&&g.allocator_stats.free_calls==0);
    void *p=guest_malloc(&g,32);assert(p);memset(p,0x5a,32);
    void *q=guest_calloc(&g,2,24);assert(q);
    for(unsigned k=0;k<48;k++)assert(((uint8_t *)q)[k]==0);
    assert(g.allocator_stats.usable_live_bytes==80&&g.allocator_stats.usable_highwater_bytes==80);
    reject_allocations=true;
    assert(guest_realloc(&g,p,96)==NULL);
    assert(g.allocator_stats.usable_live_bytes==80&&g.allocator_stats.failed_calls==1);
    for(unsigned k=0;k<32;k++)assert(((uint8_t *)p)[k]==0x5a);
    reject_allocations=false;p=guest_realloc(&g,p,96);assert(p);
    for(unsigned k=0;k<32;k++)assert(((uint8_t *)p)[k]==0x5a);
    assert(g.allocator_stats.usable_live_bytes==144&&g.allocator_stats.usable_highwater_bytes==144);
    p=guest_realloc(&g,p,16);assert(p);
    assert(g.allocator_stats.usable_live_bytes==64&&g.allocator_stats.usable_highwater_bytes==144);
    assert(g.allocator_stats.max_successful_request==96);
    guest_free(&g,q);assert(g.allocator_stats.usable_live_bytes==16);
    assert(guest_realloc(&g,p,0)==NULL&&g.allocator_stats.usable_live_bytes==0);
    p=guest_realloc(&g,NULL,48);assert(p);guest_free(&g,p);
    assert(g.allocator_stats.allocation_calls==2&&g.allocator_stats.realloc_calls==5&&g.allocator_stats.free_calls==2);
    assert(guest_calloc(&g,SIZE_MAX,2)==NULL);
    reject_allocations=true;assert(guest_malloc(&g,64)==NULL);reject_allocations=false;
    assert(g.allocator_stats.failed_calls==3&&g.allocator_stats.allocation_calls==4);
    pocketjs_guest_allocator_stats_t a,b;
    assert(pocketjs_guest_allocator_snapshot(NULL,&a)==ESP_ERR_INVALID_ARG);
    assert(pocketjs_guest_allocator_snapshot(&g,NULL)==ESP_ERR_INVALID_ARG);
    assert(pocketjs_guest_allocator_snapshot(&g,&a)==ESP_OK);
    assert(pocketjs_guest_allocator_snapshot(&g,&b)==ESP_OK&&!memcmp(&a,&b,sizeof a));
    assert(a.usable_live_bytes==0&&a.max_successful_request==96);
#if TEST_DEVICE
    /* A failed backend allocation flushes the 16+48 cached bytes and retries;
     * failed_calls still counts one failed guest callback, not both attempts. */
    assert(a.cache_available&&a.cache_enabled&&a.cache_held_bytes==0&&a.cache_hits>0&&a.cache_flushes>0);
    p=guest_malloc(&g,48);assert(p);guest_free(&g,p);
    p=guest_malloc(&g,48);assert(p);guest_free(&g,p);
    assert(pocketjs_guest_allocator_snapshot(&g,&b)==ESP_OK&&b.cache_hits>a.cache_hits);
    task=2;memset(&b,0x73,sizeof b);a=b;
    assert(pocketjs_guest_allocator_snapshot(&g,&b)==ESP_ERR_INVALID_STATE&&!memcmp(&a,&b,sizeof a));task=1;
    block_cache_flush(&g.block_cache);
    assert(pocketjs_guest_allocator_snapshot(&g,&b)==ESP_OK&&b.cache_held_bytes==0&&b.cache_flushes>0);
#else
    assert(!a.cache_available&&!a.cache_enabled&&a.cache_held_bytes==0);
#endif
    g.allocator_stats.allocation_calls=UINT32_MAX;g.allocator_stats.realloc_calls=UINT32_MAX;
    g.allocator_stats.free_calls=UINT32_MAX;g.allocator_stats.failed_calls=UINT32_MAX;
    p=guest_malloc(&g,80);assert(p);p=guest_realloc(&g,p,96);assert(p);guest_free(&g,p);
    reject_allocations=true;assert(guest_malloc(&g,99)==NULL);reject_allocations=false;
    assert(g.allocator_stats.allocation_calls==UINT32_MAX&&g.allocator_stats.realloc_calls==UINT32_MAX&&
           g.allocator_stats.free_calls==UINT32_MAX&&g.allocator_stats.failed_calls==UINT32_MAX);
#if TEST_DEVICE
    block_cache_flush(&g.block_cache);
#endif
    assert(g.allocator_stats.usable_live_bytes==0&&backend_live==0);
    puts(TEST_DEVICE?"GUEST_ALLOCATOR_METRICS_PASS device-model usable/cache/owner/failure/realloc/saturation released=0":
                     "GUEST_ALLOCATOR_METRICS_PASS host usable/failure/realloc/saturation released=0");
#else
    /* Function pointer identity proves OFF uses the original raw callbacks,
     * rather than a counter wrapper whose branches merely happen to be off. */
    assert(guest_malloc==guest_raw_malloc&&guest_free==guest_raw_free&&guest_realloc==guest_raw_realloc);
    void *p=guest_calloc(&g,2,24);assert(p&&guest_usable_size(p)>=48);for(unsigned k=0;k<48;k++)assert(((uint8_t *)p)[k]==0);
    p=guest_realloc(&g,p,96);assert(p);guest_free(&g,p);
    pocketjs_guest_allocator_stats_t a,b;memset(&a,0x39,sizeof a);b=a;
    assert(pocketjs_guest_allocator_snapshot(&g,&b)==ESP_ERR_NOT_SUPPORTED&&!memcmp(&a,&b,sizeof a));
    assert(guest_calloc(&g,SIZE_MAX,2)==NULL);
#if TEST_DEVICE
    block_cache_flush(&g.block_cache);
#endif
    assert(backend_live==0);puts("GUEST_ALLOCATOR_METRICS_OFF_PASS raw callbacks/unsupported snapshot/released=0");
#endif
}
'''

BRIDGE = r'''
#undef NDEBUG
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "pocketjs/guest.h"
#include "quickjs.h"
#define POCKET_ERR_BUSY "BUSY"
#define POCKET_ERR_INVALID_ARGUMENT "INVALID_ARGUMENT"
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
static JSContext *js_ctx;
static bool memory_mark_active,memory_mark_busy,wrong_owner,reenter;
static uint32_t guest_generation=7;
struct pocketjs_guest {JSContext *context;};
static struct pocketjs_guest instance,*guest=&instance;
static unsigned logs,snapshots;
static char last_log[1024];
static int64_t clock_us=100;
static int64_t esp_timer_get_time(void){return clock_us++;}
static size_t heap_caps_get_free_size(unsigned c){assert(c==3);return 50000;}
static size_t heap_caps_get_largest_free_block(unsigned c){assert(c==3);return 30000;}
static size_t heap_caps_get_minimum_free_size(unsigned c){assert(c==3);return 40000;}
static JSContext *pocketjs_guest_quickjs_context(pocketjs_guest_t *g){return g->context;}
esp_err_t pocketjs_guest_allocator_snapshot(const pocketjs_guest_t *g,pocketjs_guest_allocator_stats_t *out){
    assert(g==guest);++snapshots;if(wrong_owner)return ESP_ERR_INVALID_STATE;
    *out=(pocketjs_guest_allocator_stats_t){.usable_live_bytes=100,.usable_highwater_bytes=200,
        .max_successful_request=80,.allocation_calls=5,.cache_available=true,.cache_enabled=true};return ESP_OK;
}
esp_err_t app_memory_mark(JSContext *context,const char *label);
static void record(const char *tag,const char *format,...){
    assert(!strcmp(tag,"app"));va_list ap;va_start(ap,format);vsnprintf(last_log,sizeof last_log,format,ap);va_end(ap);++logs;
    if(reenter){reenter=false;assert(app_memory_mark(js_ctx,"nested")==ESP_ERR_INVALID_STATE);}
}
#define ESP_LOGI record
static JSValue pocket_api_throw(JSContext *ctx,const char *code,const char *op,const char *message,bool retry,const char *outcome){
    (void)op;(void)retry;(void)outcome;return JS_ThrowTypeError(ctx,"%s:%s",code,message);
}
@MARK@
@BRIDGE@
static void run(JSContext *ctx,const char *source){
    JSValue result=JS_Eval(ctx,source,strlen(source),"memory-mark-test",JS_EVAL_TYPE_GLOBAL);
    if(JS_IsException(result)){JSValue e=JS_GetException(ctx);const char *s=JS_ToCString(ctx,e);fprintf(stderr,"%s\n",s?s:"exception");JS_FreeCString(ctx,s);JS_FreeValue(ctx,e);assert(0);}
    JS_FreeValue(ctx,result);
}
int main(void){
    JSRuntime *rt=JS_NewRuntime();assert(rt);js_ctx=JS_NewContext(rt);assert(js_ctx);instance.context=js_ctx;
    JSValue global=JS_GetGlobalObject(js_ctx);JS_SetPropertyStr(js_ctx,global,"mark",JS_NewCFunction(js_ctx,js_memory_mark,"mark",1));JS_FreeValue(js_ctx,global);
    run(js_ctx,"if(mark('scene.ready')!==undefined)throw Error('result object');");
    assert(logs==1&&strstr(last_log,"MMK scene.ready gen=7")&&strstr(last_log,"live=100 high=200 maxreq=80"));
    assert(strstr(last_log,"mn_scope=trace_window")&&strstr(last_log,"arena_largest=NA gc_count=NA gc_us=NA"));
    run(js_ctx,"for(const x of ['', 'a'.repeat(33), 'bad label', '\\n', '\\0x', 'é', '界', {}, null, 2]){let caught=false;try{mark(x)}catch(e){caught=String(e).includes('INVALID_ARGUMENT')}if(!caught)throw Error('bad label '+x)}");
    run(js_ctx,"for(const args of [[],['x','y']]){let caught=false;try{mark(...args)}catch(e){caught=String(e).includes('INVALID_ARGUMENT')}if(!caught)throw Error('arity')}");
    assert(logs==1&&!memory_mark_active&&!memory_mark_busy);
    run(js_ctx,"mark('a'.repeat(32))");assert(logs==2);
    wrong_owner=true;run(js_ctx,"let caught=false;try{mark('owner')}catch(e){caught=String(e).includes('BUSY')}if(!caught)throw Error('owner')");
    assert(logs==2&&!memory_mark_active&&!memory_mark_busy);wrong_owner=false;
    reenter=true;assert(app_memory_mark(js_ctx,"outer")==ESP_OK&&logs==3&&!memory_mark_busy);
    memory_mark_active=true;JSValue arg=JS_NewString(js_ctx,"busy"),r=js_memory_mark(js_ctx,JS_UNDEFINED,1,&arg);assert(JS_IsException(r));
    JS_FreeValue(js_ctx,r);JSValue e=JS_GetException(js_ctx);JS_FreeValue(js_ctx,e);JS_FreeValue(js_ctx,arg);memory_mark_active=false;
    assert(logs==3&&app_memory_mark(NULL,"none")==ESP_ERR_INVALID_STATE);
    const unsigned before=snapshots;assert(app_memory_mark(js_ctx,"invalid space")==ESP_ERR_INVALID_ARG&&snapshots==before);
    JS_FreeContext(js_ctx);JS_FreeRuntime(rt);
    puts("MEMORY_MARK_PASS actual QuickJS primitive/bounded/invalid/owner/reentry/resultUndefined checkpoint/NA/scope");
}
'''


def function(path, name):
    text = (ROOT / path).read_text(encoding='utf-8')
    found = re.search(r'(?ms)^(?:static )?(?:esp_err_t|JSValue) ' + re.escape(name) + r'\([^;{]*\{.*?^}', text)
    if not found:
        raise RuntimeError(f'production function not found: {name}')
    return found[0]


def sources():
    source = GUEST.read_text(encoding='utf-8')
    first = source.index('static void *guest_raw_malloc(')
    split = source.index('\n#else\n', first)
    end = source.index('\n#endif\n', split)
    counter_start = source.rfind('#if POCKET_GUEST_ALLOCATOR_METRICS', 0, source.index('static void allocator_count('))
    counter_end = source.index('static const JSMallocFunctions GUEST_ALLOCATOR', counter_start)
    # Device allocator helpers have their production predicates/rounding too.
    helper_start = source.index('static inline size_t guest_tlsf_round(')
    helper_end = source.index('static void *guest_raw_malloc(', helper_start)
    return (source[split + len('\n#else\n'):end],
            source[helper_start:helper_end] + source[first:split],
            source[counter_start:counter_end])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--quickjs', action='store_true', help='also compile real QuickJS and test production checkpoint/JS bridge')
    options = parser.parse_args()
    host, device, counters = sources()
    with tempfile.TemporaryDirectory(prefix='guest-allocator-metrics-') as tmp:
        for mode, raw in enumerate((host, device)):
            for enabled in (0, 1):
                source, binary = Path(tmp) / f'case{mode}-{enabled}.c', Path(tmp) / f'case{mode}-{enabled}'
                source.write_text(C.replace('@RAW@', raw).replace('@COUNTERS@', counters))
                args = [os.environ.get('CC', 'gcc'), '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer', f'-DTEST_DEVICE={mode}',
                        f'-DPOCKET_GUEST_ALLOCATOR_METRICS={enabled}']
                for directory in ('components/pocketjs_guest/include', 'components/quickjs-ng/quickjs-ng', 'tools/hostshim'):
                    args += ['-I', str(ROOT / directory)]
                args += [str(source), str(ROOT / 'components/pocketjs_guest/src/block_cache.c'), '-o', str(binary)]
                subprocess.run(args, check=True)
                subprocess.run([str(binary)], check=True)
        if options.quickjs:
            qjs = ROOT / 'components/quickjs-ng/quickjs-ng'
            common = [os.environ.get('CC', 'gcc'), '-std=gnu11', '-O1', '-g', '-D_GNU_SOURCE', '-DQUICKJS_NG_BUILD',
                      '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-I' + str(qjs),
                      '-I' + str(ROOT / 'components/pocketjs_guest/include'), '-I' + str(ROOT / 'tools/hostshim')]
            objects = []
            for name in ('dtoa', 'libregexp', 'libunicode', 'quickjs', 'quickjs-vm'):
                obj = Path(tmp) / f'{name}.o'
                subprocess.run(common + ['-c', str(qjs / f'{name}.c'), '-o', str(obj)], check=True)
                objects.append(str(obj))
            source, binary = Path(tmp) / 'bridge.c', Path(tmp) / 'bridge'
            source.write_text(BRIDGE.replace('@MARK@', function('main/app_session.c', 'app_memory_mark'))
                              .replace('@BRIDGE@', function('main/pocket/pocket_app.c', 'js_memory_mark')))
            subprocess.run(common + ['-Wall', '-Wextra', '-Werror', str(source), *objects, '-lm', '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
