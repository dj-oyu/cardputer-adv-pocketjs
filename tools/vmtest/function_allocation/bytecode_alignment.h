/* Included after the real engine, so no private layouts are mirrored here. */
#ifdef FA_EXPECT_ALIGN8
_Static_assert(sizeof(JSFunctionBytecode) == 80, "32-bit function header uses derived pointers and GC-unused flags");
_Static_assert(_Alignof(JSFunctionBytecode) == 8, "32-bit function header is value-aligned");
#endif

static const char fa_bc_source[] =
    "(function outer(a = 3) {"
    "const text = 'atom', value = 1.25, big = 12345678901234567890n;"
    "function inner(b) { try { return text + ':' + (a+b+value) + ':' + big; } finally {} }"
    "function* gen() { yield inner(4); } return gen().next().value; })()";
static const char fa_bc_answer[] = "atom:8.25:12345678901234567890";

static JSValue fa_bc_compile(JSContext *ctx, const char *source)
{
    return JS_Eval(ctx, source, strlen(source), "bytecode-alignment.js",
                   JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
}

static unsigned fa_bc_inspect(JSValueConst value)
{
    assert(JS_VALUE_GET_TAG(value) == JS_TAG_FUNCTION_BYTECODE);
    JSFunctionBytecode *b = JS_VALUE_GET_PTR(value);
    assert((uintptr_t)b % _Alignof(JSFunctionBytecode) == 0);
    assert((b->cpool_count == 0) == (fa_cpool(b) == NULL));
    unsigned found = b->cpool_count != 0;
    if (b->cpool_count) {
        assert((uint8_t *)fa_cpool(b) == (uint8_t *)b + sizeof(*b));
        assert((uintptr_t)fa_cpool(b) % _Alignof(JSValue) == 0);
        for (int i = 0; i < b->cpool_count; i++) {
            JSValue item = fa_cpool(b)[i]; /* typed load: UBSan catches the old bug */
            if (JS_VALUE_GET_TAG(item) == JS_TAG_FUNCTION_BYTECODE)
                found += fa_bc_inspect(item);
        }
    }
    return found;
}

static void fa_bc_expect(JSContext *ctx, JSValueConst function, const char *answer)
{
    JSValue result = JS_EvalFunction(ctx, JS_DupValue(ctx, function));
    assert(!JS_IsException(result));
    const char *text = JS_ToCString(ctx, result);
    assert(text != NULL && strcmp(text, answer) == 0);
    JS_FreeCString(ctx, text);
    JS_FreeValue(ctx, result);
}

typedef struct FABCOOM {
    size_t attempts, fail_at, hits, live;
} FABCOOM;

/* Return requested usable sizes, like vmrun: libc's reuse-dependent slack
   otherwise changes parser resize counts between dense failure iterations. */
typedef union FABCHeader {
    max_align_t alignment;
    struct { size_t size; FABCOOM *owner; void *base; uintptr_t magic; } info;
} FABCHeader;
_Static_assert(_Alignof(FABCHeader) >= _Alignof(JSValue), "test allocations preserve value alignment");

static bool fa_bc_fail(FABCOOM *state)
{
    state->attempts++;
    if (state->fail_at && state->attempts == state->fail_at) {
        state->hits++;
        return true;
    }
    return false;
}

#define FA_MAGIC ((uintptr_t)0xa13579bd)
static FABCHeader fa_bc_header(const void *ptr)
{
    FABCHeader h;
    memcpy(&h, (const uint8_t *)ptr - sizeof(h), sizeof(h));
    assert(h.info.magic == FA_MAGIC); /* rejects any shifted/interior pointer */
    return h;
}
static void *fa_bc_malloc(void *opaque, size_t size)
{
    FABCOOM *state = opaque;
    unsigned forced = fa_function_allocation_active ? fa_force_shift : 0;
    if (fa_function_allocation_active) fa_function_raw_attempts++;
    /* Consume even a failed function allocation before its OOM exception
       allocates ordinary objects. Only this one raw block may be shifted. */
    fa_function_allocation_active = 0;
    if (fa_bc_fail(state)) return NULL;
    assert(size <= SIZE_MAX - sizeof(FABCHeader) - 4);
    void *base = def_malloc_funcs.js_malloc(NULL, sizeof(FABCHeader) + 4 + size);
    if (!base) return NULL;
    uint8_t *ptr = (uint8_t *)base + sizeof(FABCHeader) + forced;
    FABCHeader h = {0};
    h.info.size=size; h.info.owner=state; h.info.base=base; h.info.magic=FA_MAGIC;
    memcpy(ptr - sizeof(h), &h, sizeof(h));
    state->live++;
    return ptr;
}
static void *fa_bc_calloc(void *opaque, size_t count, size_t size)
{
    assert(!count || size <= SIZE_MAX/count);
    void *p=fa_bc_malloc(opaque,count*size);
    if(p) memset(p,0,count*size);
    return p;
}
static void fa_bc_free(void *opaque, void *ptr)
{
    if (!ptr) return;
    FABCOOM *state=opaque;
    FABCHeader h=fa_bc_header(ptr);
    assert(state->live && h.info.owner==state);
    state->live--;
    def_malloc_funcs.js_free(NULL,h.info.base);
}
static void *fa_bc_realloc(void *opaque, void *ptr, size_t size)
{
    if(!ptr) return fa_bc_malloc(opaque,size);
    if(!size) { fa_bc_free(opaque,ptr); return NULL; }
    FABCHeader h=fa_bc_header(ptr);
    void *p=fa_bc_malloc(opaque,size);
    if(!p) return NULL;
    memcpy(p,ptr,size<h.info.size?size:h.info.size);
    fa_bc_free(opaque,ptr);
    return p;
}
static size_t fa_bc_usable(const void *ptr)
{ return ptr ? fa_bc_header(ptr).info.size : 0; }

/* Arm only after context setup; each failure point belongs to the compiler,
   writer or reader phase rather than unrelated intrinsic initialization. */
static size_t fa_bc_oom_one(int phase, size_t fail_at, const uint8_t *blob, size_t len)
{
    FABCOOM state = {0};
    JSMallocFunctions mf = {fa_bc_calloc, fa_bc_malloc, fa_bc_free,
                           fa_bc_realloc, fa_bc_usable};
    JSRuntime *rt = JS_NewRuntime2(&mf, &state);
    assert(rt != NULL);
    JSContext *ctx = JS_NewContext(rt);
    assert(ctx != NULL);
    JSValue input = JS_UNDEFINED;
    if (phase == 2) {
        input = fa_bc_compile(ctx, fa_bc_source);
        assert(!JS_IsException(input));
    }
    state.attempts = state.hits = 0;
    state.fail_at = fail_at;
    JSValue result;
    if (phase == 0) {
        result = fa_bc_compile(ctx, fa_bc_source);
    } else if (phase == 1) {
        result = JS_ReadObject(ctx, blob, len, JS_READ_OBJ_BYTECODE);
    } else {
        size_t size = SIZE_MAX;
        fprintf(stderr, "bytecode writer fail_at=%zu\n", fail_at);
        uint8_t *encoded = JS_WriteObject(ctx, &size, input, JS_WRITE_OBJ_BYTECODE);
        if (fail_at) {
            if (encoded || size || !JS_HasException(ctx))
                fprintf(stderr,"writer contract failure point=%zu encoded=%d size=%zu exception=%d\n",fail_at,encoded!=NULL,size,JS_HasException(ctx));
            assert(encoded == NULL && size == 0 && JS_HasException(ctx));
        } else {
            assert(encoded != NULL && size != 0 && !JS_HasException(ctx));
        }
        result = encoded ? JS_UNDEFINED : JS_EXCEPTION;
        js_free(ctx, encoded);
    }
    size_t attempts = state.attempts;
    state.fail_at = 0;
    assert(!fail_at || state.hits == 1);
    if (JS_IsException(result)) {
        assert(fail_at != 0);
        JSValue error=JS_GetException(ctx);
        if (phase==2) {
            const char *message=JS_ToCString(ctx,error);
            assert(message && strstr(message,"out of memory"));
            JS_FreeCString(ctx,message);
        }
        JS_FreeValue(ctx,error);
    } else {
        if (phase != 2) {
            assert(fa_bc_inspect(result) >= 2);
            JS_RunGC(rt);
            fa_bc_expect(ctx, result, fa_bc_answer);
        }
        JS_FreeValue(ctx, result);
    }
    JS_FreeValue(ctx, input);
    JS_RunGC(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    assert(state.live == 0); /* includes atoms, source, cpool and exceptions */
    return attempts;
}

static void fa_bc_test(JSRuntime *rt)
{
    JSContext *ctx = JS_NewContext(rt);
    assert(ctx != NULL);
    uint8_t *oom_blob = NULL;
    size_t oom_len = 0;
    for (unsigned sample = 0; sample < 2; sample++) {
        JSValue compiled = fa_bc_compile(ctx, sample ? fa_bc_source : "1");
        assert(!JS_IsException(compiled));
        unsigned pools = fa_bc_inspect(compiled);
        fprintf(stderr, "bytecode sample=%u nonempty_pools=%u root_count=%d\n", sample, pools,
                ((JSFunctionBytecode *)JS_VALUE_GET_PTR(compiled))->cpool_count);
        assert(sample ? pools >= 2 : pools == 0);
        for (unsigned strip = 0; strip < 4; strip++) {
            int flags = JS_WRITE_OBJ_BYTECODE |
                ((strip & 1) ? JS_WRITE_OBJ_STRIP_SOURCE : 0) |
                ((strip & 2) ? JS_WRITE_OBJ_STRIP_DEBUG : 0);
            size_t size, copied_size;
            uint8_t *encoded = JS_WriteObject(ctx, &size, compiled, flags);
            assert(encoded != NULL);
            JSValue decoded = JS_ReadObject(ctx, encoded, size, JS_READ_OBJ_BYTECODE);
            assert(!JS_IsException(decoded));
            assert(fa_bc_inspect(decoded) == pools);
            JS_RunGC(rt);
            uint8_t *copied = JS_WriteObject(ctx, &copied_size, decoded, flags);
            assert(copied != NULL && copied_size == size && memcmp(encoded, copied, size) == 0);
            js_free(ctx, copied);
            fa_bc_expect(ctx, compiled, sample ? fa_bc_answer : "1");
            fa_bc_expect(ctx, decoded, sample ? fa_bc_answer : "1");
            JS_FreeValue(ctx, decoded);
            if (sample && strip == 0) {
                oom_blob = encoded;
                oom_len = size;
            } else {
                js_free(ctx, encoded);
            }
        }
        JS_FreeValue(ctx, compiled);
    }
    assert(oom_blob != NULL);
    for (unsigned cut = 0; cut < 3; cut++) {
        size_t len = cut == 0 ? 0 : cut == 1 ? oom_len / 2 : oom_len - 1;
        JSValue malformed = JS_ReadObject(ctx, oom_blob, len, JS_READ_OBJ_BYTECODE);
        assert(JS_IsException(malformed));
        JS_FreeValue(ctx, JS_GetException(ctx));
        JS_RunGC(rt);
    }
    size_t total = 0;
    for (int phase = 0; phase < 3; phase++) {
        size_t attempts = fa_bc_oom_one(phase, 0, oom_blob, oom_len);
        assert(attempts > 0);
        for (size_t i = 1; i <= attempts; i++) fa_bc_oom_one(phase, i, oom_blob, oom_len);
        total += attempts;
        printf("bytecode OOM phase=%s points=%zu PASS\n",
               phase == 0 ? "compiler" : phase == 1 ? "reader" : "writer", attempts);
    }
    js_free(ctx, oom_blob);
    JS_RunGC(rt);
    JS_FreeContext(ctx);
    printf("bytecode alignment PASS header=%zu align=%zu value_align=%zu roundtrips=8 OOM=%zu\n",
           sizeof(JSFunctionBytecode), _Alignof(JSFunctionBytecode), _Alignof(JSValue), total);
}

static void fa_shift_gc_test(void)
{
    FABCOOM state={0};
    JSMallocFunctions mf={fa_bc_calloc,fa_bc_malloc,fa_bc_free,fa_bc_realloc,fa_bc_usable};
    JSRuntime *rt=JS_NewRuntime2(&mf,&state); assert(rt);
    JSContext *ctx=JS_NewContext(rt); assert(ctx);
    size_t live=state.live;
    uint8_t shift=0xff;
    size_t attempts=fa_function_raw_attempts;
    assert(js_alloc_function_bytecode(ctx,SIZE_MAX,false,&shift)==NULL);
    /* Constructing the OOM exception can itself allocate. The oversized
       function request must never reach the underlying allocator. */
    assert(fa_function_raw_attempts==attempts && shift==0xff);
    assert(JS_HasException(ctx)); JS_FreeValue(ctx,JS_GetException(ctx));
    JSValue f=fa_bc_compile(ctx,fa_bc_source); assert(!JS_IsException(f));
    JSFunctionBytecode *b=JS_VALUE_GET_PTR(f);
    assert(b->header.dummy1==fa_force_shift);
    JS_FreeValue(ctx,f); /* normal reference count destructor */
    const char *cycle="(function(){let object={};object.f=function(){return object};})()";
    JSValue v=JS_Eval(ctx,cycle,strlen(cycle),"cycle.js",JS_EVAL_TYPE_GLOBAL);
    assert(!JS_IsException(v)); JS_FreeValue(ctx,v); JS_RunGC(rt);
    v=JS_Eval(ctx,cycle,strlen(cycle),"teardown.js",JS_EVAL_TYPE_GLOBAL);
    assert(!JS_IsException(v)); JS_FreeValue(ctx,v);
    JS_FreeContext(ctx); JS_FreeRuntime(rt); assert(!state.live);
    printf("forced raw shift=%u overflow/RC/cycle/teardown PASS\n",fa_force_shift);
}
