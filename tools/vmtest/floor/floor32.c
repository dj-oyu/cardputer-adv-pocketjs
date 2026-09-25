// The guest's startup floor, computed for the device: built -m32
// -malign-double (the Xtensa layout), with an allocator that charges what the
// device's guest_malloc charges -- the tlsf block length, i.e. the request
// rounded up to 4 B with a 12 B minimum (guest.c; tlsf adjust_request_size).
// tlsf also spends a 4 B header per block that js= does not see; reported as
// `hdr` so the heap's own view can be had by adding it.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "quickjs.h"

typedef struct { size_t usable; } hdr_t;
static size_t total, blocks;

static size_t tlsf_len(size_t n) {
    n = (n + 3) & ~(size_t)3;
    return n < 12 ? 12 : n;
}
static void *m_malloc(void *o, size_t n) {
    (void)o;
    if (!n) return NULL;
    size_t u = tlsf_len(n);
    hdr_t *h = malloc(sizeof(hdr_t) + u);
    if (!h) return NULL;
    h->usable = u; total += u; blocks++;
    return h + 1;
}
static void m_free(void *o, void *p) {
    (void)o;
    if (!p) return;
    hdr_t *h = (hdr_t *)p - 1;
    total -= h->usable; blocks--;
    free(h);
}
static void *m_calloc(void *o, size_t c, size_t n) {
    void *p = m_malloc(o, c * n);
    if (p) memset(p, 0, c * n);
    return p;
}
static size_t m_usable(const void *p) { return p ? ((const hdr_t *)p - 1)->usable : 0; }
static void *m_realloc(void *o, void *p, size_t n) {
    if (!p) return m_malloc(o, n);
    if (!n) { m_free(o, p); return NULL; }
    void *q = m_malloc(o, n);
    if (!q) return NULL;
    size_t old = m_usable(p);
    memcpy(q, p, old < n ? old : n);
    m_free(o, p);
    return q;
}
static const JSMallocFunctions MF = {m_calloc, m_malloc, m_free, m_realloc, m_usable};

static void line(const char *name, size_t t0, size_t b0, JSRuntime *rt, JSMemoryUsage *u0) {
    JSMemoryUsage u;
    JS_ComputeMemoryUsage(rt, &u);
#define D(f) (long)(u.f - (u0 ? u0->f : 0))
    printf("%-14s js=%6ld blocks=%4ld hdr=%5ld | obj %3ld prop %4ld shape %3ld atom %3ld atomB %5ld\n",
           name, (long)(total - t0), (long)(blocks - b0), (long)(blocks - b0) * 4,
           D(obj_count), D(prop_count), D(shape_count), D(atom_count), D(atom_size));
}

static int eval_(JSContext *c) { return JS_AddIntrinsicEval(c); }
static int rxc(JSContext *c) { JS_AddIntrinsicRegExpCompiler(c); return 0; }

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("sizeof: JSValue=%zu ptr=%zu\n", sizeof(JSValue), sizeof(void *));
    struct { const char *name; int (*add)(JSContext *); } t[] = {
        {"TypedArrays", JS_AddIntrinsicTypedArrays}, {"MapSet", JS_AddIntrinsicMapSet},
        {"Date", JS_AddIntrinsicDate}, {"DOMException", JS_AddIntrinsicDOMException},
        {"RegExp", JS_AddIntrinsicRegExp}, {"Promise", JS_AddIntrinsicPromise},
        {"WeakRef", JS_AddIntrinsicWeakRef}, {"Proxy", JS_AddIntrinsicProxy},
        {"JSON", JS_AddIntrinsicJSON}, {"Eval", eval_}, {"RegExpCompiler", rxc},
        {"BigInt", JS_AddIntrinsicBigInt},
    };
    JSMemoryUsage u0;
    size_t t0 = total, b0 = blocks;
    JSRuntime *rt = JS_NewRuntime2(&MF, NULL);
    line("runtime", t0, b0, rt, NULL);
    JS_ComputeMemoryUsage(rt, &u0);
    size_t t1 = total, b1 = blocks;
    JSContext *full = JS_NewContext(rt);
    line("full_context", t1, b1, rt, &u0);
    printf("TOTAL_FLOOR js=%ld\n", (long)(total - t0));
    JS_FreeContext(full);
    JS_FreeRuntime(rt);

    rt = JS_NewRuntime2(&MF, NULL);
    JS_ComputeMemoryUsage(rt, &u0);
    t1 = total; b1 = blocks;
    JSContext *c = JS_NewContextRaw(rt);
    JS_AddIntrinsicBaseObjects(c);
    line("raw+Base", t1, b1, rt, &u0);
    JS_FreeContext(c);
    JS_FreeRuntime(rt);

    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
        rt = JS_NewRuntime2(&MF, NULL);
        c = JS_NewContextRaw(rt);
        JS_AddIntrinsicBaseObjects(c);
        JS_ComputeMemoryUsage(rt, &u0);
        t1 = total; b1 = blocks;
        t[i].add(c);
        line(t[i].name, t1, b1, rt, &u0);
        if (i + 1 < sizeof t / sizeof t[0]) { JS_FreeContext(c); JS_FreeRuntime(rt); }
    }
    return 0;
}
