/* Internal invariants need the real parser, not a second implementation. */
#include "../../components/quickjs-ng/quickjs-ng/quickjs.c"
#include "fault_allocator.h"

static void unit_trim(void) {
    TestAllocator a = {0};
    JSRuntime *rt = JS_NewRuntime2(&test_mf, &a);
    JSContext *ctx = JS_NewContext(rt);
    assert(ctx);
    JSOOMCanary canary;
    JS_TakeOOMCanary(rt, &canary);
    for (int reject = 0; reject < 2; reject++) {
        JSFunctionDef fd = {.ctx = ctx};
        js_dbuf_init(ctx, &fd.byte_code);
        assert(dbuf_putstr(&fd.byte_code, "bytecode payload") == 0);
        assert(dbuf_claim(&fd.byte_code, 200) == 0);
        fd.label_count = 3; fd.label_size = 20;
        fd.var_count = 2; fd.var_size = 20;
        fd.cpool_count = 1; fd.cpool_size = 20;
        fd.label_slots = js_mallocz(ctx, fd.label_size * sizeof(*fd.label_slots));
        fd.vars = js_mallocz(ctx, fd.var_size * sizeof(*fd.vars));
        fd.cpool = js_mallocz(ctx, fd.cpool_size * sizeof(*fd.cpool));
        assert(fd.label_slots && fd.vars && fd.cpool);
        fd.label_slots[2].pos = 42; fd.vars[1].scope_next = 7;
        fd.cpool[0] = JS_NewInt32(ctx, 123);
        void *old_bc = fd.byte_code.buf, *old_vars = fd.vars;
        JSValue sentinel = JS_NewObject(ctx); assert(!JS_IsException(sentinel));
        JS_Throw(ctx, JS_DupValue(ctx, sentinel));
        uint32_t oom_before = rt->oom_count;
        size_t before = rt->malloc_state.malloc_size;
        a.deny_shrinks = reject;
        js_parse_trim_capacity(&fd);
        a.deny_shrinks = 0;
        assert(fd.byte_code.size == strlen("bytecode payload"));
        assert(!memcmp(fd.byte_code.buf, "bytecode payload", fd.byte_code.size));
        assert(fd.label_slots[2].pos == 42 && fd.vars[1].scope_next == 7);
        assert(JS_VALUE_GET_INT(fd.cpool[0]) == 123);
        if (reject) {
            assert(fd.byte_code.buf == old_bc && fd.vars == old_vars);
            assert(fd.var_size == 20 && fd.cpool_size == 20 && fd.label_size == 20);
            assert(rt->malloc_state.malloc_size == before);
        } else {
            assert(fd.byte_code.buf != old_bc && fd.vars != old_vars);
            assert(fd.byte_code.allocated_size == fd.byte_code.size);
            assert(fd.var_size == 2 && fd.cpool_size == 1 && fd.label_size == 3);
            assert(rt->malloc_state.malloc_size < before);
        }
        assert(rt->malloc_state.malloc_size == a.live + a.blocks * MALLOC_OVERHEAD);
        JS_TakeOOMCanary(rt, &canary); assert(canary.count == 0);
        assert(rt->oom_count == oom_before && JS_HasException(ctx));
        JSValue kept_exception = JS_GetException(ctx);
        assert(JS_VALUE_GET_PTR(kept_exception) == JS_VALUE_GET_PTR(sentinel));
        JS_FreeValue(ctx, kept_exception); JS_FreeValue(ctx, sentinel);
        /* Later compilation passes may grow every trimmed array again. */
        assert(dbuf_putstr(&fd.byte_code, " larger later pass") == 0);
        assert(new_label_fd(&fd) == 3);
        assert(add_var(ctx, &fd, JS_ATOM_NULL) == 2);
        JSParseState s = {.ctx = ctx, .cur_func = &fd};
        assert(cpool_add(&s, JS_NewInt32(ctx, 456)) == 1);
        assert(JS_VALUE_GET_INT(fd.cpool[1]) == 456);
        /* An errored DynBuf remains errored and preserves its owned block. */
        dbuf_set_error(&fd.byte_code);
        old_bc = fd.byte_code.buf;
        js_parse_trim_capacity(&fd);
        assert(dbuf_error(&fd.byte_code) && fd.byte_code.buf == old_bc);
        dbuf_free(&fd.byte_code);
        js_free(ctx, fd.label_slots); js_free(ctx, fd.vars); js_free(ctx, fd.cpool);
    }
    JSFunctionDef empty = {.ctx = ctx};
    js_dbuf_init(ctx, &empty.byte_code);
    assert(dbuf_claim(&empty.byte_code, 30) == 0);
    empty.label_size = empty.var_size = empty.cpool_size = 2;
    empty.label_slots = js_malloc(ctx, 2 * sizeof(*empty.label_slots));
    empty.vars = js_malloc(ctx, 2 * sizeof(*empty.vars));
    empty.cpool = js_malloc(ctx, 2 * sizeof(*empty.cpool));
    a.deny_all = 1;
    js_parse_trim_capacity(&empty);
    a.deny_all = 0;
    assert(!empty.byte_code.buf && !empty.vars && !empty.label_slots && !empty.cpool);
    assert(!empty.byte_code.allocated_size && !empty.var_size && !empty.label_size && !empty.cpool_size);
    JS_TakeOOMCanary(rt, &canary); assert(canary.count == 0);
    test_destroy(rt, ctx, &a);
    puts("parser trim units: moving shrink, rejection, zero, sticky error, regrowth PASS");
}

static const char *programs[] = {
    "(function outer(a) { let x = 7; function inner(b) { let y = b * 2; return () => x + y + a; } return inner(13)(); })(9)",
    "(function() { class Base { value() { return 7; } } class C extends Base { #x = 35; constructor() { super(); } get x() { return this.#x; } f = () => this.x + super.value(); static { this.extra = 1; } } return new C().f(); })()",
    "(function() { function f(a) { eval('var x = 30'); return () => x + arguments[0]; } return f(12)(); })()",
    "(function() { function many(){ let a0=0,a1=1,a2=2,a3=3,a4=4,a5=5,a6=6,a7=7,a8=8,a9=9,a10=10,a11=11,a12=12,a13=13,a14=14,a15=15,a16=16,a17=17,a18=18,a19=19,a20=20,a21=21,a22=22,a23=23,a24=24,a25=25,a26=26,a27=27,a28=28,a29=29,a30=30,a31=31; return () => a31+a11; } return many()(); })()",
    "(function() { function* g(a = 6, ...r) { try { yield a; return r[0]; } finally { let f = () => 1; f(); } } let q = g(6, 36); return q.next().value + q.next().value; })()"
};

static size_t last_compile_attempts;
static uint8_t *compile_image(JSContext *ctx, const char *src, size_t *len, JSValue *out) {
    TestAllocator *a = ctx->rt->malloc_state.opaque;
    size_t begin = a->attempts;
    *out = JS_Eval(ctx, src, strlen(src), "parser-trim.js", JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    last_compile_attempts = a->attempts - begin;
    a->fail_at = 0; /* Inject into the parser, never the serializer. */
    if (JS_IsException(*out)) return NULL;
    /* Existing late debug-table OOM can omit metadata without changing code.
     * Compare executable bytecode under faults; the separate differential
     * script compares complete images without faults. */
    return JS_WriteObject(ctx, len, *out, JS_WRITE_OBJ_BYTECODE | JS_WRITE_OBJ_STRIP_DEBUG);
}

static void sweep_compile(void) {
    size_t points = 0, optional = 0, failures = 0;
    for (size_t s = 0; s < countof(programs); s++) {
        TestAllocator ref = {0};
        JSRuntime *rr = JS_NewRuntime2(&test_mf, &ref);
        JSContext *cc = JS_NewContext(rr);
        JS_SetMaxStackSize(rr, 0);
        size_t len = 0;
        JSValue fn;
        uint8_t *image = compile_image(cc, programs[s], &len, &fn);
        assert(image && !JS_IsException(fn));
        size_t attempts = last_compile_attempts;
        uint8_t *expected = malloc(len); assert(expected);
        memcpy(expected, image, len); js_free(cc, image);
        JS_FreeValue(cc, fn);
        test_destroy(rr, cc, &ref);
        for (size_t n = 1; n <= attempts; n++) {
            TestAllocator a = {0};
            JSRuntime *rt = JS_NewRuntime2(&test_mf, &a);
            JSContext *ctx = JS_NewContext(rt);
            JS_SetMaxStackSize(rt, 0);
            a.fail_at = a.attempts + n;
            size_t got_len = 0;
            uint8_t *got = compile_image(ctx, programs[s], &got_len, &fn);
            a.fail_at = 0;
            if (got) {
                if (len != got_len || memcmp(expected, got, len)) {
                    fprintf(stderr, "bytecode mismatch source=%zu fail=%zu shrink_fail=%zu len=%zu expected=%zu\n", s, n, a.failed_shrinks, got_len, len);
                    abort();
                }
                js_free(ctx, got);
                JSValue result = JS_EvalFunction(ctx, fn);
                int32_t result_num;
                assert(!JS_IsException(result));
                assert(JS_ToInt32(ctx, &result_num, result) == 0 && result_num == 42);
                JS_FreeValue(ctx, result);
            } else {
                failures++;
                JS_FreeValue(ctx, fn);
                test_clear_exception(ctx);
            }
            optional += a.failed_shrinks;
            /* Reuse the same parser/runtime after any failed compile/write. */
            JSValue value = JS_Eval(ctx, "6*7", 3, "reuse.js", JS_EVAL_TYPE_GLOBAL);
            int32_t answer;
            assert(!JS_IsException(value));
            assert(JS_ToInt32(ctx, &answer, value) == 0 && answer == 42);
            JS_FreeValue(ctx, value);
            test_destroy(rt, ctx, &a);
            points++;
        }
        free(expected);
    }
    assert(optional > 0);
    printf("parser allocation sweep: %zu points, %zu failures handled, %zu shrink rejections, debug-stripped bytecode, execution and reuse PASS\n", points, failures, optional);
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    unit_trim();
    sweep_compile();
    return 0;
}
