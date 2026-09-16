// Exercise every parked boundary with host references gone and GC in between.
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "quickjs.h"
#include "quickjs-vm.h"
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#else
#include <pthread.h>
#include <time.h>
#endif

static int jobs;
static void check_int(JSContext *ctx, const char *expr, int expected);
#ifdef CONFIG_POCKET_VM_TCO
#include "tco_device_check.h"
#endif

typedef struct {
    JSRuntime *rt;
    atomic_bool go, done;
#ifdef ESP_PLATFORM
    esp_timer_handle_t timer;
#else
    pthread_t thread;
#endif
} request_producer;
static request_producer *active_producer;

static void producer_pause(void) {
#ifdef ESP_PLATFORM
    vTaskDelay(1);
#else
    const struct timespec delay = {.tv_nsec = 1000000};
    nanosleep(&delay, NULL);
#endif
}

#ifdef ESP_PLATFORM
static void producer_fire(void *opaque) {
    request_producer *p = opaque;
#else
static void *producer_fire(void *opaque) {
    request_producer *p = opaque;
    while (!atomic_load_explicit(&p->go, memory_order_acquire)) producer_pause();
    producer_pause();
#endif
    JS_VMRequestYield(p->rt);
    atomic_store_explicit(&p->done, true, memory_order_release);
#ifndef ESP_PLATFORM
    return NULL;
#endif
}

static JSValue start_request(JSContext *ctx, JSValueConst self,
                             int argc, JSValueConst *argv) {
    (void)ctx; (void)self; (void)argc; (void)argv;
#ifdef ESP_PLATFORM
    assert(esp_timer_start_once(active_producer->timer, 1000) == ESP_OK);
#else
    atomic_store_explicit(&active_producer->go, true, memory_order_release);
#endif
    return JS_UNDEFINED;
}

static int producer_watchdog(JSRuntime *rt, void *opaque) {
    (void)rt;
    return ++*(unsigned *)opaque > 10000;
}

static void check_concurrent_requests(void) {
    for (int round = 0; round < 10; round++) {
        JSRuntime *rt = JS_NewRuntime();
        JSContext *ctx = JS_NewContext(rt);
        request_producer p = {.rt = rt};
        atomic_init(&p.go, false);
        atomic_init(&p.done, false);
        active_producer = &p;
#ifdef ESP_PLATFORM
        const esp_timer_create_args_t args = {
            .callback = producer_fire, .arg = &p, .name = "vm-test-yield"};
        assert(esp_timer_create(&args, &p.timer) == ESP_OK);
#else
        assert(pthread_create(&p.thread, NULL, producer_fire, &p) == 0);
#endif
        JSValue global = JS_GetGlobalObject(ctx);
        JS_SetPropertyStr(ctx, global, "startRequest",
                          JS_NewCFunction(ctx, start_request, "startRequest", 0));
        JS_FreeValue(ctx, global);
        unsigned polls = 0;
        JS_SetInterruptHandler(rt, producer_watchdog, &polls);
        const char *src = "startRequest();for(;;){}";
        JSValue v = JS_VMEval(ctx, src, strlen(src), "concurrent", JS_EVAL_TYPE_GLOBAL);
        assert(JS_VMSuspended(rt));
        JS_FreeValue(ctx, v);
        while (!atomic_load_explicit(&p.done, memory_order_acquire)) producer_pause();
#ifdef ESP_PLATFORM
        assert(esp_timer_delete(p.timer) == ESP_OK);
#else
        assert(pthread_join(p.thread, NULL) == 0);
#endif
        active_producer = NULL;
        JS_VMTerminate(rt);
        v = JS_VMResume(ctx);
        assert(JS_IsException(v) && !JS_VMSuspended(rt));
        JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
    }
    puts("lifecycle concurrent requests=10 OK");
}
static int watchdog(JSRuntime *rt, void *opaque) {
    (void)rt;
    ++*(int *)opaque;
    return 1;
}
static JSValue job(JSContext *ctx, int argc, JSValueConst *argv) {
    (void)ctx; (void)argc; (void)argv;
    jobs++;
    return JS_UNDEFINED;
}

static JSValue request_yield(JSContext *ctx, JSValueConst self,
                             int argc, JSValueConst *argv) {
    (void)self; (void)argc; (void)argv;
    JS_VMRequestYield(JS_GetRuntime(ctx));
    return JS_UNDEFINED;
}

static void check_requests(void) {
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "requestYield",
                      JS_NewCFunction(ctx, request_yield, "requestYield", 0));
    JS_FreeValue(ctx, global);
    check_int(ctx, "globalThis.progress=0", 0);
    // Coalescing and a class-B stop before any bytecode has run.
    JS_VMRequestYield(rt);
    JS_VMRequestYield(rt);
    JSValue v = JS_VMEval(ctx, "progress=1", 10, "request", JS_EVAL_TYPE_GLOBAL);
    assert(JS_VMSuspended(rt));
    JS_FreeValue(ctx, v);
    v = JS_VMResume(ctx);
    assert(!JS_VMSuspended(rt) && !JS_IsException(v));
    JS_FreeValue(ctx, v);
    check_int(ctx, "progress", 1);
    JS_VMRequestYield(rt);
    JS_VMClearYield(rt);
    v = JS_VMEval(ctx, "progress=2", 10, "cleared", JS_EVAL_TYPE_GLOBAL);
    assert(!JS_VMSuspended(rt) && !JS_IsException(v));
    JS_FreeValue(ctx, v);
    // A native map callback cannot park: retain the request until its
    // caller reaches a retired branch after all callback side effects.
    const char *src = "[1,2,3].map(x=>{requestYield();for(let i=0;i<4;i++)progress++;});"
                      "while(progress<20)progress++;";
    v = JS_VMEval(ctx, src, strlen(src), "native-hold", JS_EVAL_TYPE_GLOBAL);
    assert(JS_VMSuspended(rt));
    JS_FreeValue(ctx, v);
    // Read a data property without entering JS while the chain is parked.
    global = JS_GetGlobalObject(ctx);
    v = JS_GetPropertyStr(ctx, global, "progress");
    int32_t n;
    assert(JS_ToInt32(ctx, &n, v) == 0 && n >= 14 && n < 20);
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, global);
    v = JS_VMResume(ctx);
    assert(!JS_VMSuspended(rt) && !JS_IsException(v));
    JS_FreeValue(ctx, v);
    check_int(ctx, "progress", 20);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    puts("lifecycle requests coalesce/clear/native-hold OK");
}

static void clear_exception(JSContext *ctx) {
    JS_FreeValue(ctx, JS_GetException(ctx));
}

static void check_int(JSContext *ctx, const char *expr, int expected) {
    int32_t actual;
    JSValue v = JS_Eval(ctx, expr, strlen(expr), "check", JS_EVAL_TYPE_GLOBAL);
    assert(!JS_IsException(v));
    assert(JS_ToInt32(ctx, &actual, v) == 0 && actual == expected);
    JS_FreeValue(ctx, v);
}

static const char setup[] =
    "globalThis.effects=0;globalThis.closed=undefined;"
    "function leaf(n){let box={n};closed=()=>box.n;"
    "try{for(let i=0;i<3;i++)box.n++;return box.n}"
    "catch(e){effects+=1000}finally{effects++}}"
    "async function middle(n){let box={n};let x=leaf(box.n);await 0;return x}"
    "function tree(n){return n?tree(n-1):middle(7)}";

static const char *sources[] = {
    "tree(5)",
    "async function later(){await 0;return tree(5)};later()",
    "async function* gen(){await 0;yield tree(5)};gen().next()",
    "Promise.resolve().then(()=>tree(5))",
    "Promise.resolve().then(async()=>tree(5))",
    "Promise.resolve({then(resolve){tree(5);resolve(42)}})",
    "queueMicrotask(()=>tree(5))",
    "function tail(n){'use strict';let box={n};globalThis.tailKeep=()=>box.n;"
    "if(n)return tail(n-1);return middle(7)};tail(5)",
};

// Cross job boundaries too: async and held-job floors are not eval results.
static JSValue advance(JSContext *ctx, JSValue value) {
    JSRuntime *rt = JS_GetRuntime(ctx);
    if (JS_VMSuspended(rt)) {
        JS_FreeValue(ctx, value);
        return JS_EXCEPTION;
    }
    if (JS_IsException(value)) return value;
    JS_FreeValue(ctx, value);
    while (JS_IsJobPending(rt)) {
        JSContext *job_ctx;
        if (JS_ExecutePendingJob(rt, &job_ctx) < 0) return JS_EXCEPTION;
        if (JS_VMSuspended(rt)) return JS_EXCEPTION;
    }
    return JS_UNDEFINED;
}

int vmtest_lifecycle(void) {
    unsigned total = 0;
#ifdef CONFIG_POCKET_VM_TCO
    check_tail_depth();
#endif
    check_requests();
    check_concurrent_requests();
    {
        JSRuntime *rt = JS_NewRuntime();
        JSContext *ctx = JS_NewContext(rt);
        int calls = 0;
        unsigned resumes = 0;
        JS_SetInterruptHandler(rt, watchdog, &calls);
        vmtest_vm_set_force_yield(rt, 1);
        JSValue v = JS_VMEval(ctx, "for(;;){}", 9, "watchdog", JS_EVAL_TYPE_GLOBAL);
        while (JS_VMSuspended(rt) && resumes < 100000) {
            v = JS_VMResume(ctx);
            resumes++;
        }
        assert(calls == 1 && !JS_VMSuspended(rt) && JS_IsException(v));
        JSVMState *metrics = js_vm_state(rt);
        assert(metrics->susp_samples == resumes && metrics->susp_bytes_max > 0);
        assert(metrics->susp_async_frames == 0);
        clear_exception(ctx);
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        printf("lifecycle watchdog resumes=%u OK\n", resumes);
    }
    for (unsigned shape = 0; shape < sizeof(sources) / sizeof(sources[0]); shape++) {
    for (int mode = 0; mode < 5; mode++) {
        unsigned boundaries = 0;
        uint64_t peak_bytes = 0, peak_async = 0;
        for (unsigned target = 0; target < 100; target++) {
            JSRuntime *rt = JS_NewRuntime();
            JSContext *ctx = JS_NewContext(rt);
            assert(rt && ctx);
            JS_SetMemoryLimit(rt, 160 * 1024);
            JS_SetMaxStackSize(rt, 20 * 1024);
            JSValue init = JS_Eval(ctx, setup, strlen(setup), "setup", JS_EVAL_TYPE_GLOBAL);
            assert(!JS_IsException(init));
            JS_FreeValue(ctx, init);
            vmtest_vm_set_force_yield(rt, 1);
            const char *source = sources[shape];
            JSValue v = advance(ctx, JS_VMEval(ctx, source, strlen(source), "lifecycle", JS_EVAL_TYPE_GLOBAL));
            unsigned at = 0;
            while (JS_VMSuspended(rt) && at < target) {
                JS_RunGC(rt);
                v = advance(ctx, JS_VMResume(ctx));
                at++;
            }
            if (!JS_VMSuspended(rt)) {
                assert(!JS_IsException(v));
                JS_FreeValue(ctx, v);
                vmtest_vm_set_force_yield(rt, 0);
                JS_FreeContext(ctx);
                JS_FreeRuntime(rt);
                break;
            }
            assert(JS_IsException(v));
            JSVMOrigin origin = JS_VMSuspendedOrigin(rt);
            JSVMState *metrics = js_vm_state(rt);
            assert(metrics && metrics->susp_samples > 0);
            assert(metrics->susp_bytes_max > 0 || metrics->susp_async_frames > 0);
            if (origin == JS_VM_ORIGIN_JOB_ASYNC)
                assert(metrics->susp_async_frames > 0);
            if (metrics->susp_bytes_max > peak_bytes) peak_bytes = metrics->susp_bytes_max;
            if (metrics->susp_async_frames > peak_async) peak_async = metrics->susp_async_frames;
            boundaries++;
            JS_RunGC(rt);
            // A rejected drain must not consume or execute a queued entry.
            assert(JS_EnqueueJob(ctx, job, 0, NULL) == 0);
            JSContext *failed = NULL;
            int before = jobs;
            assert(JS_ExecutePendingJob(rt, &failed) == -1 && failed == ctx);
            assert(jobs == before && JS_IsJobPending(rt));
            clear_exception(ctx);
            JSValue global = JS_GetGlobalObject(ctx);
            JSValue effects = JS_GetPropertyStr(ctx, global, "effects");
            int32_t before_effects = 0;
            if (!JS_IsUndefined(effects))
                assert(JS_ToInt32(ctx, &before_effects, effects) == 0);
            JS_FreeValue(ctx, effects);
            JS_FreeValue(ctx, global);
            if (mode == 0) {
                vmtest_vm_set_force_yield(rt, 0);
                v = advance(ctx, JS_VMResume(ctx));
                assert(!JS_IsException(v) && !JS_VMSuspended(rt));
                JS_FreeValue(ctx, v);
                check_int(ctx, "effects", 1);
                check_int(ctx, "closed()", 10);
            } else if (mode == 1 || mode == 4) {
                // Error construction may fail; termination must still bypass
                // catch/finally and unwind across the flat async owner.
                if (mode == 4) JS_SetMemoryLimit(rt, 1);
                JS_VMTerminate(rt);
                v = JS_VMResume(ctx);
                assert(!JS_VMSuspended(rt));
                assert(JS_IsException(v) || origin == JS_VM_ORIGIN_JOB_ASYNC);
                if (JS_IsException(v)) clear_exception(ctx);
                else JS_FreeValue(ctx, v);
                JS_SetMemoryLimit(rt, 160 * 1024);
                vmtest_vm_set_force_yield(rt, 0);
                check_int(ctx, "globalThis.effects||0", before_effects);
            } else if (mode == 2) {
                JS_VMDiscard(rt);
                JS_VMDiscard(rt);
                assert(!JS_VMSuspended(rt));
                vmtest_vm_set_force_yield(rt, 0);
                check_int(ctx, "40+2", 42);
                check_int(ctx, "globalThis.effects||0", before_effects);
                check_int(ctx, "typeof closed==='function' ? +(closed()>=7 && closed()<=10) : 1", 1);
            }
            JS_RunGC(rt);
            JS_FreeContext(ctx);
            JS_FreeRuntime(rt);  // mode 3 exercises implicit teardown.
#ifdef ESP_PLATFORM
            vTaskDelay(1);
#endif
        }
        assert(boundaries > 10 && boundaries < 100);
        assert(peak_bytes > 0 && peak_async > 0);
        printf("lifecycle shape=%u mode=%d boundaries=%u susp_bytes_max=%llu susp_async_frames=%llu OK\n",
               shape, mode, boundaries, (unsigned long long)peak_bytes, (unsigned long long)peak_async);
        total += boundaries;
    }
    }
    printf("lifecycle total=%u OK\n", total);
    return 0;
}

#ifndef ESP_PLATFORM
int main(void) { return vmtest_lifecycle(); }
#else
void vmtest_lifecycle_device(void) {
    extern void vmtest_guest_lifecycle(void);
    size_t before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    int64_t start = esp_timer_get_time();
    vmtest_guest_lifecycle();
    vmtest_lifecycle();
    printf("VM_LIFECYCLE_OK free_before=%u free_after=%u largest=%u elapsed_us=%lld\n",
           (unsigned)before, (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
           (long long)(esp_timer_get_time() - start));
}
#endif
