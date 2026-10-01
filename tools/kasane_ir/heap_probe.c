/* The guest heap one script keeps and peaks at, charged as the device
 * charges (TLSF lengths, as tools/vmtest/retain_cost.c), host -m32 only.
 *
 *   heap_probe FILE.js
 *
 * evaluates FILE.js in a fresh runtime with four globals:
 *   __heap()       bytes charged now,
 *   __gc()         JS_RunGC,
 *   __peak_reset() starts a new peak at the current charge,
 *   __peak()       the highest charge since the last reset,
 *   __print(s)     one line on stdout.
 * tools/kasane_ir/derby_heap.py builds it against the m32 quickjs objects
 * of tools/games/run_derby.py and measures prog()'s registration peak.
 */
#include "quickjs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { size_t n; size_t pad; } hdr_t;
static size_t cur, peak;
static size_t tlsf_len(size_t n) { n = (n + 3) & ~(size_t)3; return n < 12 ? 12 : n; }
static void charge(size_t add, size_t sub) { cur += add; cur -= sub; if (cur > peak) peak = cur; }
static void *pk_malloc(void *o, size_t n) {
    (void)o;
    if (!n) return NULL;
    hdr_t *h = malloc(sizeof *h + tlsf_len(n));
    if (!h) return NULL;
    h->n = n; charge(tlsf_len(n), 0);
    return h + 1;
}
static void *pk_calloc(void *o, size_t c, size_t n) {
    void *p = pk_malloc(o, c * n);
    if (p) memset(p, 0, c * n);
    return p;
}
static void pk_free(void *o, void *p) {
    (void)o;
    if (!p) return;
    hdr_t *h = (hdr_t *)p - 1;
    charge(0, tlsf_len(h->n));
    free(h);
}
static void *pk_realloc(void *o, void *p, size_t n) {
    if (!p) return pk_malloc(o, n);
    if (!n) { pk_free(o, p); return NULL; }
    hdr_t *h = (hdr_t *)p - 1;
    size_t old = tlsf_len(h->n);
    hdr_t *q = realloc(h, sizeof *q + tlsf_len(n));
    if (!q) return NULL;
    q->n = n; charge(tlsf_len(n), old);
    return q + 1;
}
static size_t pk_usable(const void *p) { return p ? tlsf_len(((const hdr_t *)p - 1)->n) : 0; }
static const JSMallocFunctions MF = {pk_calloc, pk_malloc, pk_free, pk_realloc, pk_usable};
static JSRuntime *rt;

static JSValue js_heap(JSContext *c, JSValueConst t, int n, JSValueConst *v) {
    (void)t; (void)n; (void)v; return JS_NewInt64(c, (int64_t)cur);
}
static JSValue js_gc(JSContext *c, JSValueConst t, int n, JSValueConst *v) {
    (void)c; (void)t; (void)n; (void)v; JS_RunGC(rt); return JS_UNDEFINED;
}
static JSValue js_peak_reset(JSContext *c, JSValueConst t, int n, JSValueConst *v) {
    (void)c; (void)t; (void)n; (void)v; peak = cur; return JS_UNDEFINED;
}
static JSValue js_peak(JSContext *c, JSValueConst t, int n, JSValueConst *v) {
    (void)t; (void)n; (void)v; return JS_NewInt64(c, (int64_t)peak);
}
static JSValue js_print(JSContext *c, JSValueConst t, int n, JSValueConst *v) {
    (void)t;
    const char *s = n ? JS_ToCString(c, v[0]) : NULL;
    if (s) { puts(s); JS_FreeCString(c, s); }
    return JS_UNDEFINED;
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "heap_probe FILE.js\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    static char src[1 << 17];
    size_t n = fread(src, 1, sizeof src - 1, f);
    fclose(f); src[n] = 0;
    rt = JS_NewRuntime2(&MF, NULL);
    JSContext *ctx = JS_NewContext(rt);
    JSValue g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "__heap", JS_NewCFunction(ctx, js_heap, "__heap", 0));
    JS_SetPropertyStr(ctx, g, "__gc", JS_NewCFunction(ctx, js_gc, "__gc", 0));
    JS_SetPropertyStr(ctx, g, "__peak_reset", JS_NewCFunction(ctx, js_peak_reset, "__peak_reset", 0));
    JS_SetPropertyStr(ctx, g, "__peak", JS_NewCFunction(ctx, js_peak, "__peak", 0));
    JS_SetPropertyStr(ctx, g, "__print", JS_NewCFunction(ctx, js_print, "__print", 1));
    JS_FreeValue(ctx, g);
    JSValue r = JS_Eval(ctx, src, n, argv[1], JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        JSValue e = JS_GetException(ctx);
        const char *s = JS_ToCString(ctx, e);
        fprintf(stderr, "exception: %s\n", s ? s : "?");
        return 1;
    }
    JS_FreeValue(ctx, r);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 0;
}
