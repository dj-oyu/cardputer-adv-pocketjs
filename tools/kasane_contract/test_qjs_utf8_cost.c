/* Measure the actual QuickJS allocator calls made by JS_ToCStringLen.
 * This is a host classification test, not a device cycle benchmark. */
#include "quickjs.h"
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef union { max_align_t align; size_t size; } block_header;
static size_t calls, bytes;

static void *count_malloc(void *opaque, size_t size) {
    (void)opaque;
    if (size > SIZE_MAX - sizeof(block_header)) return NULL;
    block_header *p = malloc(sizeof(*p) + size);
    if (!p) return NULL;
    p->size = size;
    calls++;
    bytes += size;
    return p + 1;
}
static void *count_calloc(void *opaque, size_t count, size_t size) {
    if (size && count > SIZE_MAX / size) return NULL;
    void *p = count_malloc(opaque, count * size);
    if (p) memset(p, 0, count * size);
    return p;
}
static void count_free(void *opaque, void *ptr) {
    (void)opaque;
    if (ptr) free((block_header *)ptr - 1);
}
static void *count_realloc(void *opaque, void *ptr, size_t size) {
    (void)opaque;
    if (!ptr) return count_malloc(NULL, size);
    if (!size) { count_free(NULL, ptr); return NULL; }
    if (size > SIZE_MAX - sizeof(block_header)) return NULL;
    block_header *p = realloc((block_header *)ptr - 1, sizeof(*p) + size);
    if (!p) return NULL;
    p->size = size;
    calls++;
    bytes += size;
    return p + 1;
}
static size_t count_size(const void *ptr) {
    return ptr ? ((const block_header *)ptr - 1)->size : 0;
}

static void probe(JSContext *ctx, const char *name, const char *utf8,
                  bool expect_allocation) {
    size_t input_len = strlen(utf8), output_len = 0;
    JSValue value = JS_NewStringLen(ctx, utf8, input_len);
    assert(!JS_IsException(value));
    calls = bytes = 0;
    const char *out = JS_ToCStringLen(ctx, &output_len, value);
    assert(out && output_len == input_len && memcmp(out, utf8, input_len) == 0);
    printf("%s: input=%zu allocator_calls=%zu requested_bytes=%zu\n",
           name, input_len, calls, bytes);
    assert((calls != 0) == expect_allocation);
    JS_FreeCString(ctx, out);
    JS_FreeValue(ctx, value);
}

int main(void) {
    const JSMallocFunctions allocator = {count_calloc, count_malloc, count_free,
                                         count_realloc, count_size};
    JSRuntime *rt = JS_NewRuntime2(&allocator, NULL);
    assert(rt);
    JSContext *ctx = JS_NewContext(rt);
    assert(ctx);
    probe(ctx, "ascii", "TITLE", false);
    probe(ctx, "latin1", "\xc3\xa9", true);
    probe(ctx, "japanese", "\xe3\x83\xaa\xe3\x82\xba\xe3\x83\xa0", true);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    puts("PASS QuickJS UTF-8 borrow/conversion classification");
    return 0;
}
