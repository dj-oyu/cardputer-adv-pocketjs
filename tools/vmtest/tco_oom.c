// Force the ordinary-call fallback to fail before it can enter a large callee.
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "quickjs.h"
#include "quickjs-vm.h"

static unsigned seals;
static JSValue seal(JSContext *ctx, JSValueConst self, int argc,
                    JSValueConst *argv) {
    (void)self; (void)argc; (void)argv;
    JSMemoryUsage usage;
    JS_ComputeMemoryUsage(JS_GetRuntime(ctx), &usage);
    // Leave room for an error/backtrace, but not a 600-local frame.
    JS_SetMemoryLimit(JS_GetRuntime(ctx), usage.malloc_size + 2048);
    seals++;
    return JS_UNDEFINED;
}

static void check(JSContext *ctx, const char *source, int expected) {
    JSValue v = JS_Eval(ctx, source, strlen(source), "check", JS_EVAL_TYPE_GLOBAL);
    assert(!JS_IsException(v));
    int32_t result;
    assert(JS_ToInt32(ctx, &result, v) == 0 && result == expected);
    JS_FreeValue(ctx, v);
}

int main(void) {
    for (int forced = 0; forced < 2; forced++) {
        JSRuntime *rt = JS_NewRuntime();
        assert(rt);
        JSContext *ctx = JS_NewContext(rt);
        assert(ctx);
        JS_SetMaxStackSize(rt, 1024 * 1024);
        JSValue global = JS_GetGlobalObject(ctx);
        assert(JS_SetPropertyStr(ctx, global, "seal", JS_NewCFunction(ctx, seal, "seal", 0)) >= 0);
        char source[20000];
        size_t n = (size_t)snprintf(source, sizeof(source), "function big(){'use strict';let ");
        for (int i = 0; i < 600; i++)
            n += (size_t)snprintf(source+n, sizeof(source)-n, "%sv%d=%d", i ? "," : "", i, i);
        n += (size_t)snprintf(source+n, sizeof(source)-n, ";globalThis.entered=1;return ()=>");
        for (int i = 0; i < 600; i++)
            n += (size_t)snprintf(source+n, sizeof(source)-n, "%sv%d", i ? "+" : "", i);
        n += (size_t)snprintf(source+n, sizeof(source)-n,
            "}function entry(){'use strict';let marker={value:17};"
            "globalThis.closed=()=>marker.value;seal();return big()}"
            "function floor(){return entry()}globalThis.entered=0;");
        assert(n < sizeof(source));
        JSValue init = JS_Eval(ctx, source, n, "tco-oom", JS_EVAL_TYPE_GLOBAL);
        assert(!JS_IsException(init));
        JS_FreeValue(ctx, init);
        JSValue fn = JS_GetPropertyStr(ctx, global, "floor");
        JSOOMCanary oom;
        JS_TakeOOMCanary(rt, &oom);
        seals = 0;
        vmtest_vm_set_force_yield(rt, forced);
        JSValue result = JS_VMCall(ctx, fn, JS_UNDEFINED, 0, NULL);
        unsigned resumes = 0;
        while (JS_VMSuspended(rt)) {
            JS_FreeValue(ctx, result);
            JS_RunGC(rt);
            result = JS_VMResume(ctx);
            assert(++resumes < 100);
        }
        assert(JS_IsException(result) && seals == 1);
        JS_TakeOOMCanary(rt, &oom);
        assert(oom.count > 0 && oom.first_req > 2048);
        JS_SetMemoryLimit(rt, (size_t)-1);
        vmtest_vm_set_force_yield(rt, 0);
        JSValue error = JS_GetException(ctx);
        const char *message = JS_ToCString(ctx, error);
        assert(message && strstr(message, "InternalError"));
        JS_FreeCString(ctx, message);
        JS_FreeValue(ctx, error);
        JS_FreeValue(ctx, fn);
        JS_FreeValue(ctx, global);
        JS_RunGC(rt);
        check(ctx, "entered", 0);
        check(ctx, "closed()", 17);
        check(ctx, "big()()", 179700);
        check(ctx, "(function again(){'use strict';return 42})()", 42);
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        printf("tco fallback oom forced=%d resumes=%u rejected=%zu count=%u OK\n",
               forced, resumes, oom.first_req, oom.count);
    }
}
