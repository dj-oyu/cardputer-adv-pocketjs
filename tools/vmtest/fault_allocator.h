/* Host-only moving allocator: expose stale parser pointers and account every
 * live allocation independently of the engine. The 12-byte minimum / 4-byte
 * rounding model lengths only; neither this allocator nor host64 models ESP32. */
typedef union TestHeader {
    struct { size_t size, usable; } h;
    max_align_t align;
} TestHeader;
typedef struct TestAllocator {
    size_t live, blocks, peak, attempts, fail_at, shrinks, failed_shrinks;
    int deny_shrinks, deny_all;
} TestAllocator;
static size_t test_usable(const void *p) {
    return p ? (((const TestHeader *)p) - 1)->h.usable : 0;
}
static void *test_alloc_raw(TestAllocator *a, size_t n) {
    size_t usable = (n + 3) & ~(size_t)3;
    if (usable < 12) usable = 12;
    TestHeader *h = malloc(sizeof(*h) + usable);
    if (!h) return NULL;
    h->h.size = n; h->h.usable = usable;
    a->live += usable; a->blocks++;
    if (a->live > a->peak) a->peak = a->live;
    return h + 1;
}
static void *test_malloc(void *opaque, size_t n) {
    TestAllocator *a = opaque;
    a->attempts++;
    if (a->deny_all || a->attempts == a->fail_at) return NULL;
    return n ? test_alloc_raw(a, n) : NULL;
}
static void test_free(void *opaque, void *p) {
    if (!p) return;
    TestAllocator *a = opaque;
    a->live -= test_usable(p); a->blocks--;
    free(((TestHeader *)p) - 1);
}
static void *test_calloc(void *opaque, size_t count, size_t n) {
    if (n && count > SIZE_MAX / n) return NULL;
    void *p = test_malloc(opaque, count * n);
    if (p) memset(p, 0, count * n);
    return p;
}
static void *test_realloc(void *opaque, void *p, size_t n) {
    TestAllocator *a = opaque;
    if (!p) return test_malloc(opaque, n);
    if (!n) { test_free(opaque, p); return NULL; }
    /* js_realloc2 exposes allocator slack to engine-owned arrays. */
    size_t old = test_usable(p);
    int shrink = n < test_usable(p);
    a->attempts++;
    if (shrink) a->shrinks++;
    if (a->deny_all || a->attempts == a->fail_at || (shrink && a->deny_shrinks)) {
        if (shrink) a->failed_shrinks++;
        return NULL;
    }
    void *q = test_alloc_raw(a, n);
    if (!q) return NULL;
    memcpy(q, p, old < n ? old : n);
    test_free(opaque, p);
    return q;
}
static const JSMallocFunctions test_mf = {
    .js_calloc = test_calloc, .js_malloc = test_malloc, .js_free = test_free,
    .js_realloc = test_realloc, .js_malloc_usable_size = test_usable
};
static void test_clear_exception(JSContext *ctx) {
    JS_FreeValue(ctx, JS_GetException(ctx));
}
static void test_destroy(JSRuntime *rt, JSContext *ctx, TestAllocator *a) {
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    assert(a->blocks == 0 && a->live == 0);
}
