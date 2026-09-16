// Same interpreter image, alternate idle-runtime dispatch. Timing excludes
// compilation, GC, logging and path verification. No VM instrumentation armed.
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#else
#include <time.h>
#endif
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"
#include "quickjs-vm.h"

static uintptr_t low, high;
static int callback_checks;
static uint64_t now_ns(void)
{
#ifdef ESP_PLATFORM
    return (uint64_t)esp_timer_get_time() * 1000;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
#endif
}

static JSValue sample(JSContext *ctx, JSValueConst self,
                      int argc, JSValueConst *argv)
{
    (void)self; (void)argc; (void)argv;
    uintptr_t p = (uintptr_t)__builtin_frame_address(0);
    if (p < low) low = p;
    if (p > high) high = p;
    if (vmtest_call_mode(JS_GetRuntime(ctx), 0) != -1 ||
        vmtest_call_mode(JS_GetRuntime(ctx), 1) != -1 ||
        vmtest_call_inputs_eager(JS_GetRuntime(ctx), 0) != -1 ||
        vmtest_call_inputs_eager(JS_GetRuntime(ctx), 1) != -1)
        return JS_ThrowInternalError(ctx, "live dispatch mutation accepted");
    callback_checks++;
    return JS_UNDEFINED;
}

static int exception(JSContext *ctx)
{
    JSValue e = JS_GetException(ctx);
    const char *s = JS_ToCString(ctx, e);
    printf("CALLBENCH ERROR %s\n", s ? s : "exception");
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, e);
    return -1;
}

static const struct {
    const char *name, *source;
    int expected;
} cases[] = {
    {"loop", "(function(){let s=0;for(let i=0;i<20000;i++)s+=i;return s})", 199990000},
    {"call", "(()=>{function f(x){return x+1}return function(){let s=0;for(let i=0;i<20000;i++)s+=f(i);return s}})()", 200010000},
    {"method", "(()=>{let o={k:1,f(x){return x+this.k}};return function(){let s=0;for(let i=0;i<20000;i++)s+=o.f(i);return s}})()", 200010000},
    {"recursive", "(()=>{function f(n){return n?1+f(n-1):1}return function(){let s=0;for(let i=0;i<1000;i++)s+=f(16);return s}})()", 17000},
};

static int select_mode(JSRuntime *rt, int inputs, int mode)
{
    return vmtest_call_mode(rt, inputs ? 0 : mode) ||
           vmtest_call_inputs_eager(rt, inputs ? mode : 0);
}

static int run(int inputs)
{
    const char *names[2] = {inputs ? "lazy" : "flat", inputs ? "eager" : "recur"};
    printf("CALLBENCH KIND %s\n", inputs ? "inputs" : "dispatch");
    int status = -1;
    JSRuntime *rt = JS_NewRuntime();
    if (!rt) return -1;
    JS_SetMemoryLimit(rt, 160 * 1024);
#ifdef ESP_PLATFORM
    JS_SetMaxStackSize(rt, 20 * 1024);
#else
    JS_SetMaxStackSize(rt, 8 * 1024 * 1024);
#endif
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) { JS_FreeRuntime(rt); return -1; }
    JSValue global = JS_GetGlobalObject(ctx);
    if (JS_SetPropertyStr(ctx, global, "sample", JS_NewCFunction(ctx, sample, "sample", 0)) < 0) {
        JS_FreeValue(ctx, global);
        goto done;
    }
    JS_FreeValue(ctx, global);
    if (vmtest_call_mode(rt, -1) != -1 || vmtest_call_mode(rt, 2) != -1 ||
        vmtest_call_inputs_eager(rt, -1) != -1 || vmtest_call_inputs_eager(rt, 2) != -1)
        goto done;
    uintptr_t spans[2];
    for (int mode = 0; mode < 2; mode++) {
        if (select_mode(rt, inputs, mode)) goto done;
        low = UINTPTR_MAX; high = 0; callback_checks = 0;
        const char *source = "(function f(n){sample();if(n)f(n-1)})(16)";
        JSValue v = JS_Eval(ctx, source, strlen(source), "path.js", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(v)) { exception(ctx); goto done; }
        JS_FreeValue(ctx, v);
        spans[mode] = high - low;
        printf("CALLBENCH PATH mode=%s span=%" PRIuPTR " checks=%d\n",
               names[mode], spans[mode], callback_checks);
        if (callback_checks != 17) goto done;
    }
    if (spans[0] != 0 || (inputs ? spans[1] != 0 : spans[1] == 0)) goto done;
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        if (vmtest_call_mode(rt, 0)) goto done;
        JSValue fn = JS_Eval(ctx, cases[c].source, strlen(cases[c].source),
                             "bench.js", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(fn)) { exception(ctx); goto done; }
        // ABBA order balances drift; every timed block has two untimed warmups.
        for (int round = 0; round < 8; round++) {
            for (int slot = 0; slot < 4; slot++) {
                int mode = (slot == 1 || slot == 2) ^ (round & 1);
                if (select_mode(rt, inputs, mode)) { JS_FreeValue(ctx, fn); goto done; }
                JS_RunGC(rt);
                for (int warm = 0; warm < 3; warm++) {
                    uint64_t t = now_ns();
                    JSValue v = JS_Call(ctx, fn, JS_UNDEFINED, 0, NULL);
                    uint64_t elapsed = now_ns() - t;
                    int32_t value = 0;
                    int bad = JS_IsException(v) || JS_ToInt32(ctx, &value, v) < 0;
                    JS_FreeValue(ctx, v);
                    if (bad || value != cases[c].expected) {
                        if (bad) exception(ctx);
                        JS_FreeValue(ctx, fn); goto done;
                    }
                    if (warm == 2)
                        printf("CALLBENCH SAMPLE case=%s round=%d slot=%d mode=%s ns=%" PRIu64 " value=%" PRId32 "\n",
                               cases[c].name, round, slot, names[mode], elapsed, value);
                }
#ifdef ESP_PLATFORM
                vTaskDelay(1);
#endif
            }
        }
        JS_FreeValue(ctx, fn);
    }
    status = 0;
done:
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    printf("CALLBENCH %s\n", status ? "FAIL" : "PASS");
    return status;
}

#ifdef ESP_PLATFORM
void vmtest_callbench_device(void) { (void)run(0); }
void vmtest_callinputs_device(void) { (void)run(1); }
#else
int main(int argc, char **argv)
{
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "--inputs"))) return 2;
    return run(argc == 2) ? EXIT_FAILURE : EXIT_SUCCESS;
}
#endif
