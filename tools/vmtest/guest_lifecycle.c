// Device-level tests of the actual guest receiver, not a vmrun substitute.
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "pocketjs/guest.h"
#include "pocketjs/guest_quickjs.h"
#include "pocketjs/vm_sched.h"
#include "quickjs-vm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static JSValue request(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self; (void)argc; (void)argv;
    JS_VMRequestYield(JS_GetRuntime(ctx));
    return JS_UNDEFINED;
}

static int value(JSContext *ctx, const char *expr) {
    JSValue v = JS_Eval(ctx, expr, strlen(expr), "guest-check", JS_EVAL_TYPE_GLOBAL);
    int32_t n;
    assert(!JS_IsException(v) && JS_ToInt32(ctx, &n, v) == 0);
    JS_FreeValue(ctx, v);
    return n;
}

void vmtest_guest_lifecycle(void) {
    const char *sources[] = {
        "globalThis.frame=()=>{calls++;work()}",
        "globalThis.frame=()=>{calls++;Promise.resolve().then(work)}",
        "async function later(){await 0;work()}globalThis.frame=()=>{calls++;later()}",
        "async function* gen(){await 0;work();yield 1}globalThis.frame=()=>{calls++;gen().next()}",
    };
    unsigned cases = 0;
    for (unsigned shape = 0; shape < 4; shape++) {
      for (unsigned mode = 0; mode < 4; mode++) {
        pocketjs_guest_config_t config;
        pocketjs_guest_config_defaults(&config);
        config.heap_limit = 160 * 1024;
        config.stack_limit = 20 * 1024;
        config.prefer_psram = false;
        pocketjs_guest_t *guest = NULL;
        assert(pocketjs_guest_create(&config, &guest) == ESP_OK);
        JSContext *ctx = pocketjs_guest_quickjs_context(guest);
        JSValue global = JS_GetGlobalObject(ctx);
        JS_SetPropertyStr(ctx, global, "requestYield", JS_NewCFunction(ctx, request, "requestYield", 0));
        JS_FreeValue(ctx, global);
        assert(value(ctx, "globalThis.calls=0;globalThis.side=0;globalThis.done=0;"
                     "function work(){try{requestYield();for(let i=0;i<3;i++)side++}finally{done++}};0") == 0);
        assert(pocketjs_guest_eval(guest, sources[shape], strlen(sources[shape]), "guest") == ESP_OK);
        pocketjs_guest_frame_t input = {.struct_size = sizeof(input)};
        assert(pocketjs_guest_frame(guest, &input) == ESP_OK);
        assert(pocketjs_guest_suspended(guest) && pocketjs_guest_work_pending(guest));
        if (shape == 0) assert(!pocketjs_guest_jobs_pending(guest));
        if (shape == 1) assert(pocketjs_guest_jobs_pending(guest));
        assert(pocketjs_guest_frame(guest, &input) == ESP_ERR_INVALID_STATE);
        JS_RunGC(JS_GetRuntime(ctx));
        if (mode == 0) {
            assert(pocketjs_guest_continue(guest) == ESP_OK);
            assert(!pocketjs_guest_work_pending(guest));
            assert(value(ctx, "calls===1&&side===3&&done===1") == 1);
            pocketjs_guest_stats_t stats = {.struct_size = sizeof(stats)};
            assert(pocketjs_guest_stats(guest, &stats) == ESP_OK);
            assert(stats.frames == 1 && stats.continuations == 1);
            if (shape == 1) assert(stats.jobs == 1);
        } else if (mode == 1 || mode == 3) {
            if (mode == 3) JS_SetMaxStackSize(JS_GetRuntime(ctx), 1);
            pocketjs_guest_prepare_stop(guest);
            if (mode == 3) JS_SetMaxStackSize(JS_GetRuntime(ctx), 20 * 1024);
            assert(!pocketjs_guest_suspended(guest));
            assert(value(ctx, "side===0&&done===0") == 1);
            assert(value(ctx, "40+2") == 42); // a stop hook may now enter JS
        }
        // mode 2 destroys a live parked chain without a continuation.
        pocketjs_guest_destroy(guest);
        cases++;
        vTaskDelay(1);
      }
    }
    printf("lifecycle guest receiver cases=%u OK\n", cases);
    for (unsigned mode = 0; mode < 3; mode++) {
        pocketjs_guest_config_t config;
        pocketjs_guest_config_defaults(&config);
        config.heap_limit = 160 * 1024;
        config.stack_limit = 20 * 1024;
        config.prefer_psram = false;
        pocketjs_guest_t *guest = NULL;
        assert(pocketjs_guest_create(&config, &guest) == ESP_OK);
        JSContext *ctx = pocketjs_guest_quickjs_context(guest);
        const char *src = "globalThis.side=0;globalThis.done=0;globalThis.frame=()=>{"
                          "try{for(let i=0;i<20000;i++)side++;}finally{done++;}}";
        assert(pocketjs_guest_eval(guest, src, strlen(src), "timed-guest") == ESP_OK);
        vm_budget_t budget;
        vm_budget_begin(&budget, 100);
        pocketjs_guest_budget(guest, &budget);
        pocketjs_guest_frame_t input = {.struct_size = sizeof(input)};
        assert(pocketjs_guest_frame(guest, &input) == ESP_OK);
        assert(pocketjs_guest_suspended(guest));
        assert(pocketjs_guest_frame_total(guest) > 0);
        unsigned resumes = 0;
        if (mode == 2) {
            pocketjs_guest_prepare_stop(guest);
            assert(!pocketjs_guest_suspended(guest));
            assert(value(ctx, "done") == 0);
        } else {
            if (mode == 1) pocketjs_guest_yield_enabled(guest, false);
            while (pocketjs_guest_work_pending(guest) && resumes < 10000) {
                JS_RunGC(JS_GetRuntime(ctx));
                vm_budget_begin(&budget, 100);
                pocketjs_guest_budget(guest, &budget);
                assert(pocketjs_guest_continue(guest) == ESP_OK);
                resumes++;
            }
            if (pocketjs_guest_work_pending(guest)) {
                JSValue g = JS_GetGlobalObject(ctx);
                JSValue p = JS_GetPropertyStr(ctx, g, "side");
                int32_t side;
                assert(JS_ToInt32(ctx, &side, p) == 0);
                JS_FreeValue(ctx, p);
                JS_FreeValue(ctx, g);
                printf("lifecycle guest timer pending mode=%u resumes=%u side=%ld frame_us=%lld\n",
                       mode, resumes, (long)side, (long long)pocketjs_guest_frame_total(guest));
            }
            assert(!pocketjs_guest_work_pending(guest));
            assert(value(ctx, "side===20000&&done===1") == 1);
            if (mode == 1) assert(resumes == 1);
        }
        pocketjs_guest_destroy(guest);
        vTaskDelay(1); // esp_timer_delete frees from the timer task
        printf("lifecycle guest timer mode=%u resumes=%u OK\n", mode, resumes);
    }
}
