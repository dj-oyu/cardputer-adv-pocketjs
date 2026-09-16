// Shared host/device proof that live tail-frame capacity does not grow.
// Included only by the opt-in TCO lifecycle diagnostic.
static unsigned tail_seals;
static JSValue tail_seal_heap(JSContext *ctx, JSValueConst self, int argc,
                              JSValueConst *argv) {
    (void)self; (void)argc; (void)argv;
    tail_seals++;
    JS_SetMemoryLimit(JS_GetRuntime(ctx), 1);
    return JS_UNDEFINED;
}

static void check_tail_sealed_heap(void) {
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    assert(rt && ctx);
    JS_SetMemoryLimit(rt, 160 * 1024);
    JS_SetMaxStackSize(rt, 20 * 1024);
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "sealTailHeap",
                      JS_NewCFunction(ctx, tail_seal_heap, "sealTailHeap", 0));
    JS_FreeValue(ctx, global);
    const char *source =
        "function sealed(n){'use strict';if(n===10000)sealTailHeap();"
        "if(!n)return 42;return sealed(n-1)};sealed(10010)";
    tail_seals = 0;
    JSValue result = JS_VMEval(ctx, source, strlen(source), "tail-sealed", JS_EVAL_TYPE_GLOBAL);
    assert(!JS_IsException(result) && tail_seals == 1);
    int32_t number;
    assert(JS_ToInt32(ctx, &number, result) == 0 && number == 42);
    JS_FreeValue(ctx, result);
    JSOOMCanary oom;
    JS_TakeOOMCanary(rt, &oom);
    assert(oom.count == 0);
    JS_SetMemoryLimit(rt, 160 * 1024);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    puts("lifecycle tco sealed_heap depth=10000 oom=0 OK");
}

static void check_tail_depth(void) {
    check_tail_sealed_heap();
    const char *forms[] = {
        "function tailDepth(n){'use strict';if(!n)return 42;return tailDepth(n-1)}",
        "function tailDepth(n){'use strict';return n?tailDepth(n-1):42}",
        "function tailDepth(n){'use strict';if(!n)return 42;"
        "try{throw null}catch(e){return tailDepth(n-1)}}",
        "function tailDepth(n){'use strict';if(!n)return 42;return eval(n-1)}"
        "var eval=tailDepth;",
    };
    for (unsigned shape = 0; shape < sizeof(forms)/sizeof(forms[0]); shape++) {
    uint64_t first_bytes = 0;
    for (int round = 0; round < 2; round++) {
        const unsigned depth = round ? 10000 : 100;
        char source[256];
        snprintf(source, sizeof(source),
                 "%s;globalThis.tailResult=tailDepth(%u)", forms[shape], depth);
        JSRuntime *rt = JS_NewRuntime();
        JSContext *ctx = JS_NewContext(rt);
        assert(rt && ctx);
        JS_SetMemoryLimit(rt, 160 * 1024);
        JS_SetMaxStackSize(rt, 20 * 1024);
        vmtest_vm_set_force_yield(rt, 1);
        JSValue v = JS_VMEval(ctx, source, strlen(source), "tail-depth", JS_EVAL_TYPE_GLOBAL);
        unsigned resumes = 0;
        while (JS_VMSuspended(rt)) {
            JS_FreeValue(ctx, v);
            if (resumes % 1000 == 0) JS_RunGC(rt);
            v = JS_VMResume(ctx);
            assert(++resumes < depth * 4 + 20);
        }
        if (JS_IsException(v)) {
            JSValue error = JS_GetException(ctx);
            const char *message = JS_ToCString(ctx, error);
            fprintf(stderr, "tail-depth depth=%u resumes=%u error=%s\n",
                    depth, resumes, message ? message : "<unprintable>");
            JS_FreeCString(ctx, message);
            JS_FreeValue(ctx, error);
            assert(!JS_IsException(v));
        }
        JS_FreeValue(ctx, v);
        JSVMState *state = js_vm_state(rt);
        const uint64_t bytes = state->susp_bytes_max;
        assert(bytes > 0 && state->susp_samples == resumes && resumes > depth);
        if (!round) first_bytes = bytes;
        else assert(bytes == first_bytes);
        vmtest_vm_set_force_yield(rt, 0);
        check_int(ctx, "tailResult", 42);
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        printf("lifecycle tco shape=%u depth=%u resumes=%u susp_bytes_max=%llu OK\n",
               shape, depth, resumes, (unsigned long long)bytes);
    }
    }
}
