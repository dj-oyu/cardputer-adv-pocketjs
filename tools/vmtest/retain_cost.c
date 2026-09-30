/* What compiled scripts keep in the guest heap, and what precompiling them
 * would keep instead (host only, -m32: the device's 8 B JSValue).
 *
 *   retain_cost MODE FILE...
 *
 * compiles every FILE (JS_EVAL_TYPE_GLOBAL | COMPILE_ONLY) into one runtime
 * and keeps the functions, then prints the charged bytes (TLSF lengths, as
 * the device charges them) and JS_ComputeMemoryUsage's parts. MODE:
 *   src    the functions as the parser left them (the firmware's path);
 *   bc     each written with JS_WriteObject(BYTECODE) and read back with
 *          JS_ReadObject, the compiled one freed (precompiled bytecode with
 *          its line tables);
 *   strip  the same, written with STRIP_DEBUG | STRIP_SOURCE.
 * The write buffer is freed before measuring (JS_ReadObject copies it: the
 * bytecode is rewritten for the runtime's atoms, docs/vm/eval-peak.md 3.5).
 * These are the scripts' functions before they run: the top-level bodies
 * are freed once they have run, the functions they declare stay.
 * tools/vmtest/retain_cost.py builds and runs it.
 */
#include "quickjs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { size_t n; size_t pad; } hdr_t;
static size_t cur;
static size_t tlsf_len(size_t n) { n = (n + 3) & ~(size_t)3; return n < 12 ? 12 : n; }
static void *pk_malloc(void *o, size_t n) {
    (void)o;
    if (!n) return NULL;
    hdr_t *h = malloc(sizeof *h + tlsf_len(n));
    if (!h) return NULL;
    h->n = n; cur += tlsf_len(n);
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
    cur -= tlsf_len(h->n);
    free(h);
}
static void *pk_realloc(void *o, void *p, size_t n) {
    if (!p) return pk_malloc(o, n);
    if (!n) { pk_free(o, p); return NULL; }
    hdr_t *h = (hdr_t *)p - 1;
    size_t old = tlsf_len(h->n);
    hdr_t *q = realloc(h, sizeof *q + tlsf_len(n));
    if (!q) return NULL;
    cur -= old; q->n = n; cur += tlsf_len(n);
    return q + 1;
}
static size_t pk_usable(const void *p) { return p ? tlsf_len(((const hdr_t *)p - 1)->n) : 0; }
static const JSMallocFunctions MF = {pk_calloc, pk_malloc, pk_free, pk_realloc, pk_usable};

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) exit(2);
    b[n] = 0;
    fclose(f);
    *len = (size_t)n;
    return b;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "retain_cost src|bc|strip FILE...\n"); return 2; }
    const char *mode = argv[1];
    const int flags = !strcmp(mode, "strip") ? JS_WRITE_OBJ_BYTECODE | JS_WRITE_OBJ_STRIP_DEBUG | JS_WRITE_OBJ_STRIP_SOURCE
                                             : JS_WRITE_OBJ_BYTECODE;
    JSRuntime *rt = JS_NewRuntime2(&MF, NULL);
    JSContext *ctx = JS_NewContext(rt);
    JS_RunGC(rt);
    const size_t before = cur;
    JSValue keep[64];
    int n = 0;
    for (int i = 2; i < argc && n < 64; i++) {
        size_t len;
        char *src = slurp(argv[i], &len);
        JSValue fn = JS_Eval(ctx, src, len, argv[i], JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
        free(src);
        if (JS_IsException(fn)) { fprintf(stderr, "compile failed: %s\n", argv[i]); return 1; }
        if (strcmp(mode, "src")) {
            size_t bl;
            uint8_t *buf = JS_WriteObject(ctx, &bl, fn, flags);
            JS_FreeValue(ctx, fn);
            JS_RunGC(rt);
            fn = JS_ReadObject(ctx, buf, bl, JS_READ_OBJ_BYTECODE);
            js_free(ctx, buf);
            if (JS_IsException(fn)) { fprintf(stderr, "read failed: %s\n", argv[i]); return 1; }
        }
        keep[n++] = fn;
    }
    JS_RunGC(rt);
    JSMemoryUsage m;
    JS_ComputeMemoryUsage(rt, &m);
    printf("%s: charged %zu B; atoms %lld (%lld B), bytecode functions %lld (%lld B), code %lld B, "
           "pc2line %lld (%lld B), objects %lld (%lld B), strings %lld (%lld B), memory used %lld B\n",
           mode, cur - before, (long long)m.atom_count, (long long)m.atom_size, (long long)m.js_func_count,
           (long long)m.js_func_size, (long long)m.js_func_code_size, (long long)m.js_func_pc2line_count,
           (long long)m.js_func_pc2line_size, (long long)m.obj_count, (long long)m.obj_size,
           (long long)m.str_count, (long long)m.str_size, (long long)m.memory_used_size);
    for (int i = 0; i < n; i++) JS_FreeValue(ctx, keep[i]);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 0;
}
