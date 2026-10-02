/* Inspect backing ownership while executing the production GC and writers. */
#include "../../components/quickjs-ng/quickjs-ng/quickjs.c"
#include "fault_allocator.h"

static size_t limit_for_threshold(size_t wanted) {
    size_t limit = wanted * 8 / 7;
    while (limit - (limit >> 3) < wanted) limit++;
    assert(limit - (limit >> 3) == wanted);
    return limit;
}
static JSValue empty_with_capacity(JSContext *ctx, uint32_t n) {
    JSValue v = JS_NewArray(ctx);
    assert(!JS_IsException(v));
    for (uint32_t i = 0; i < n; i++)
        assert(JS_SetPropertyUint32(ctx, v, i, JS_NewInt32(ctx, i)) >= 0);
    assert(JS_SetProperty(ctx, v, JS_ATOM_length, JS_NewInt32(ctx, 0)) >= 0);
    JSObject *p = JS_VALUE_GET_OBJ(v);
    assert(p->fast_array && p->u.array.count == 0 && p->u.array.u1.size >= n);
    return v;
}
static void pressure_matrix(bool include_faults) {
    uint32_t sizes[] = {1, 2, 3, 16, 512, 4096};
    for (size_t i = 0; i < countof(sizes); i++) {
        TestAllocator a = {0};
        JSRuntime *rt = JS_NewRuntime2(&test_mf, &a);
        JSContext *ctx = JS_NewContext(rt); assert(ctx);
        JSValue v = empty_with_capacity(ctx, sizes[i]);
        JSObject *p = JS_VALUE_GET_OBJ(v);
        void *old = p->u.array.u.values;
        uint32_t capacity = p->u.array.u1.size;
        /* Settle lazy creation and any unrelated cycles before measuring. */
        JS_SetMemoryLimit(rt, 0); JS_RunGC(rt);
        assert(p->u.array.u.values == old && p->u.array.u1.size == capacity);
        size_t used = rt->malloc_state.malloc_size;
        JS_SetMemoryLimit(rt, limit_for_threshold(used + 1));
        JS_RunGC(rt);
        assert(p->u.array.u.values == old && p->u.array.u1.size == capacity);
        JSOOMCanary canary; JS_TakeOOMCanary(rt, &canary);
        size_t reclaimed = test_usable(old) + MALLOC_OVERHEAD;
        size_t attempts = a.attempts;
        JS_SetMemoryLimit(rt, limit_for_threshold(used));
        if (include_faults) a.deny_all = 1;
        JS_RunGC(rt);
        assert(a.attempts == attempts);
        assert(p->u.array.u.values == NULL && p->u.array.u1.size == 0 && p->u.array.count == 0);
        assert(rt->malloc_state.malloc_size == used - reclaimed);
        JS_TakeOOMCanary(rt, &canary); assert(canary.count == 0);
        if (include_faults) {
            /* Failure to regrow must keep the reclaimed representation owned. */
            assert(JS_SetPropertyUint32(ctx, v, 0, JS_NewInt32(ctx, 7)) < 0);
            assert(p->u.array.count == 0 && !p->u.array.u.values && !p->u.array.u1.size);
            /* The existing indexed-add OOM path has already grown length.
             * It must remain a valid hole, exactly as on a fresh empty array. */
            assert(JS_VALUE_GET_INT(p->prop[0].u.value) == 1);
            test_clear_exception(ctx);
        }
        a.deny_all = 0; JS_SetMemoryLimit(rt, 0);
        assert(JS_SetPropertyUint32(ctx, v, 0, JS_NewInt32(ctx, 42)) >= 0);
        assert(p->u.array.count == 1 && p->u.array.u1.size >= 1);
        JSValue got = JS_GetPropertyUint32(ctx, v, 0);
        assert(JS_VALUE_GET_INT(got) == 42); JS_FreeValue(ctx, got);
        JS_FreeValue(ctx, v);
        test_destroy(rt, ctx, &a);
    }
    puts(include_faults ?
         "array trim thresholds: unlimited/below/exact, capacities 1/2/3/16/512/4096, no allocation, OOM/regrow PASS" :
         "array trim functional thresholds: unlimited/below/exact, capacities 1/2/3/16/512/4096, no allocation, regrow PASS");
}
static void semantic_matrix(void) {
    TestAllocator a = {0};
    JSRuntime *rt = JS_NewRuntime2(&test_mf, &a);
    JSContext *ctx = JS_NewContext(rt); assert(ctx);
    JS_SetMaxStackSize(rt, 0);
    JSValue empty = empty_with_capacity(ctx, 64);
    JSValue live = JS_NewArray(ctx);
    assert(JS_SetPropertyUint32(ctx, live, 0, JS_NewInt32(ctx, 17)) >= 0);
    JSObject *ep = JS_VALUE_GET_OBJ(empty), *lp = JS_VALUE_GET_OBJ(live);
    void *live_values = lp->u.array.u.values;
    /* A fast array can have zero stored elements and a nonzero holey length. */
    assert(JS_SetProperty(ctx, empty, JS_ATOM_length, JS_NewInt32(ctx, 99)) >= 0);
    JS_SetMemoryLimit(rt, 0); JS_RunGC(rt);
    /* Pressure is sampled before cycle collection. Even when reclaiming
     * this cycle relieves it, the empty-array pass still follows safely. */
    JSValue cycle = JS_NewObject(ctx);
    assert(JS_SetPropertyStr(ctx, cycle, "self", JS_DupValue(ctx, cycle)) >= 0);
    char padding[4096]; memset(padding, 'x', sizeof(padding));
    assert(JS_SetPropertyStr(ctx, cycle, "padding", JS_NewStringLen(ctx, padding, sizeof(padding))) >= 0);
    JS_FreeValue(ctx, cycle);
    size_t before_cycles = rt->malloc_state.malloc_size;
    JS_SetMemoryLimit(rt, limit_for_threshold(before_cycles));
    JS_RunGC(rt);
    assert(rt->malloc_state.malloc_size < before_cycles - sizeof(padding));
    assert(!ep->u.array.u.values && ep->u.array.count == 0);
    assert(JS_VALUE_GET_INT(ep->prop[0].u.value) == 99);
    assert(lp->u.array.u.values == live_values && lp->u.array.count == 1);
    JS_SetMemoryLimit(rt, 0);
    JSValue global = JS_GetGlobalObject(ctx);
    assert(JS_SetPropertyStr(ctx, global, "trimmed", JS_DupValue(ctx, empty)) >= 0);
    JS_FreeValue(ctx, global);
    const char *src = "(function(){ let a=trimmed; if (0 in a || a.length!==99) throw Error('holes'); a.push(42); if(a[99]!==42||a.length!==100||0 in a)throw Error('push'); a.length=0; a.unshift(7); if(a.pop()!==7)throw Error('reuse'); a[0]=1; a.length=0; Object.defineProperty(Array.prototype,'0',{set(v){this.seen=v},configurable:true}); try { a[0]=9; if(a.seen!==9||a.length!==0)throw Error('setter'); }finally{delete Array.prototype[0]} return 42; })()";
    JSValue result = JS_Eval(ctx, src, strlen(src), "array-trim.js", JS_EVAL_TYPE_GLOBAL);
    assert(!JS_IsException(result) && JS_VALUE_GET_INT(result) == 42);
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, empty); JS_FreeValue(ctx, live);
    test_destroy(rt, ctx, &a);
    puts("array trim semantics: cycle relief, holey length, nonempty retained, push/unshift/pop, prototype setter PASS");
}
int main(int argc, char **argv) {
    bool functional_only = argc == 2 && !strcmp(argv[1], "--functional-only");
    if (argc != 1 && !functional_only) {
        fprintf(stderr, "usage: %s [--functional-only]\n", argv[0]);
        return 2;
    }
    pressure_matrix(!functional_only);
    semantic_matrix();
    return 0;
}
