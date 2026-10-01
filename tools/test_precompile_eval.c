/* Isolated source/bytecode evaluation oracle; no firmware APIs or devices. */
#include "quickjs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

typedef union { size_t n; max_align_t align; } Header;
typedef struct { size_t remaining, live; } Alloc;
static int allowed(Alloc *a) {
    if (a->remaining == SIZE_MAX) return 1;
    if (!a->remaining) return 0;
    a->remaining--; return 1;
}
static void *alloc_malloc(void *opaque, size_t n) {
    Alloc *a = opaque;
    if (!n || n > SIZE_MAX - sizeof(Header) || !allowed(a)) return NULL;
    Header *h = malloc(sizeof(*h) + n);
    if (!h) return NULL;
    h->n = n; a->live += n; return h + 1;
}
static void alloc_free(void *opaque, void *p) {
    if (!p) return;
    Alloc *a = opaque; Header *h = (Header *)p - 1;
    a->live -= h->n; free(h);
}
static void *alloc_calloc(void *opaque, size_t c, size_t n) {
    if (n && c > SIZE_MAX / n) return NULL;
    void *p = alloc_malloc(opaque, c * n);
    if (p) memset(p, 0, c * n);
    return p;
}
static void *alloc_realloc(void *opaque, void *p, size_t n) {
    if (!p) return alloc_malloc(opaque, n);
    if (!n) { alloc_free(opaque, p); return NULL; }
    Alloc *a = opaque; Header *h = (Header *)p - 1;
    if (n > SIZE_MAX - sizeof(Header) || !allowed(a)) return NULL;
    size_t old = h->n;
    Header *q = realloc(h, sizeof(*q) + n);
    if (!q) return NULL;
    q->n = n; a->live = a->live - old + n; return q + 1;
}
static size_t alloc_usable(const void *p) {
    return p ? ((const Header *)p - 1)->n : 0;
}
static const JSMallocFunctions mf = {
    alloc_calloc, alloc_malloc, alloc_free, alloc_realloc, alloc_usable
};

static char *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END)) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    char *s = malloc((size_t)n + 1);
    if (!s) { fclose(f); return NULL; }
    if (fread(s, 1, (size_t)n, f) != (size_t)n) {
        free(s); fclose(f); return NULL;
    }
    fclose(f); s[n] = 0; *size = (size_t)n;
    return s;
}

int main(int argc, char **argv) {
    if (argc != 5 && argc != 6) return 2;
    Alloc alloc = {SIZE_MAX, 0};
    JSRuntime *rt = JS_NewRuntime2(&mf, &alloc);
    if (!rt) return 1;
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) { JS_FreeRuntime(rt); return 1; }
    JS_SetMemoryLimit(rt, (size_t)strtoull(argv[4], NULL, 10));
    if (argc == 6) alloc.remaining = (size_t)strtoull(argv[5], NULL, 10);
    size_t size;
    char *data = read_file(argv[2], &size);
    if (!data) { JS_FreeContext(ctx); JS_FreeRuntime(rt); return 2; }
    JSValue v = !strcmp(argv[1], "bc")
        ? JS_ReadObject(ctx, (uint8_t *)data, size, JS_READ_OBJ_BYTECODE)
        : JS_Eval(ctx, data, size, argv[3], JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    free(data);
    if (!JS_IsException(v)) v = JS_EvalFunction(ctx, v);
    int status = JS_IsException(v) ? 1 : 0;
    if (status) {
        v = JS_GetException(ctx);
        const char *s = JS_ToCString(ctx, v);
        printf("ERROR %s\n", s ? s : "unprintable");
        if (s) JS_FreeCString(ctx, s);
        if (JS_IsObject(v)) {
            JSValue stack = JS_GetPropertyStr(ctx, v, "stack");
            s = JS_ToCString(ctx, stack);
            if (s) { printf("%s\n", s); JS_FreeCString(ctx, s); }
            JS_FreeValue(ctx, stack);
        }
    } else {
        const char *s = JS_ToCString(ctx, v);
        printf("RESULT %s\n", s ? s : "unprintable");
        if (s) JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, v);
    /* Reporting itself can fail under a low limit. Drain any pending error
     * before destroying the realm, including the deliberately bare OOM. */
    JS_FreeValue(ctx, JS_GetException(ctx));
    /* OOM must leave the same runtime usable, not merely leak-free after
     * teardown. Restore allocation before retrying in this existing realm. */
    alloc.remaining = SIZE_MAX;
    JS_SetMemoryLimit(rt, 10000000);
    const char *recovery_source = "6 * 7";
    JSValue recovery = JS_Eval(ctx, recovery_source, strlen(recovery_source),
                               "recovery.js", JS_EVAL_TYPE_GLOBAL);
    int32_t answer = 0;
    int recovered = !JS_IsException(recovery) &&
                    JS_ToInt32(ctx, &answer, recovery) == 0 && answer == 42;
    JS_FreeValue(ctx, recovery);
    JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    if (alloc.live) {
        fprintf(stderr, "allocator leak: %zu bytes\n", alloc.live);
        return 3;
    }
    if (!recovered) {
        fprintf(stderr, "runtime did not recover after evaluation\n");
        return 4;
    }
    return status;
}
