/* What a script's compile holds at its peak, by allocation site (host only).
 *
 * docs/vm/spread-eval-oom.md: the guest's evaluation peak is set by the
 * compile (JS_Eval parses the whole script before anything runs), so this
 * charges every guest allocation of one JS_EVAL_FLAG_COMPILE_ONLY evaluation
 * the way the device does (TLSF lengths: 4 B rounding, 12 B minimum, as
 * tools/games/test_derby_host.c) and remembers, per allocation, the call
 * stack that made it. At every new peak the live bytes per stack are copied,
 * so the snapshot is the composition AT the peak, not a sum over time.
 * compile_peak.py builds this (-m32 with tools/vmtest/m32_sysroot.sh: the
 * device's 8 B JSValue and 4 B pointers), symbolises the stacks with
 * addr2line and classifies them.
 *
 *   compile_peak FILE.js OUTDIR [--read BYTECODE]
 *
 * writes OUTDIR/peak.tsv (stack id, live bytes at the peak, live blocks),
 * OUTDIR/stacks.tsv (stack id, return addresses), OUTDIR/series.tsv (event
 * number, charged bytes, stack id) and OUTDIR/summary.txt. --read evaluates a
 * JS_WriteObject image instead (the precompiled-bytecode case): its read and
 * its evaluation are what is measured. With --write FILE, the compiled
 * script is also written to FILE with JS_WriteObject.
 */
#include "quickjs.h"
#include <execinfo.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEPTH 14
#define MAX_STACKS 8192

typedef struct { size_t n; uint32_t sid; uint32_t pad[2]; } hdr_t;
typedef struct { void *pc[DEPTH]; int depth; } stack_t_;

static stack_t_ stacks[MAX_STACKS];
/* Stack 0 is everything allocated before tracking starts (the realm). */
static int nstacks = 1;
static size_t live[MAX_STACKS], live_n[MAX_STACKS];
static size_t snap[MAX_STACKS], snap_n[MAX_STACKS];
static size_t cur, peak, events;
static int tracking;
static FILE *series;

static size_t tlsf_len(size_t n) { n = (n + 3) & ~(size_t)3; return n < 12 ? 12 : n; }

static uint32_t stack_id(void) {
    void *pc[DEPTH + 3];
    int d = backtrace(pc, DEPTH + 3);
    /* Drop this function and the allocator hook. */
    int skip = d > 2 ? 2 : 0;
    d -= skip;
    for (int i = 0; i < nstacks; i++)
        if (stacks[i].depth == d && !memcmp(stacks[i].pc, pc + skip, d * sizeof(void *)))
            return (uint32_t)i;
    if (nstacks == MAX_STACKS) return MAX_STACKS - 1;
    memcpy(stacks[nstacks].pc, pc + skip, d * sizeof(void *));
    stacks[nstacks].depth = d;
    return (uint32_t)nstacks++;
}

static void charge(hdr_t *h, int sign) {
    size_t l = tlsf_len(h->n);
    if (sign > 0) { cur += l; live[h->sid] += l; live_n[h->sid]++; }
    else { cur -= l; live[h->sid] -= l; live_n[h->sid]--; }
    if (!tracking) return;
    events++;
    if (series && (events % 16 == 0 || sign > 0))
        fprintf(series, "%zu\t%zu\t%u\n", events, cur, h->sid);
    if (cur > peak) {
        peak = cur;
        memcpy(snap, live, nstacks * sizeof(size_t));
        memcpy(snap_n, live_n, nstacks * sizeof(size_t));
    }
}

static void *pk_malloc(void *o, size_t n) {
    (void)o;
    if (!n) return NULL;
    hdr_t *h = malloc(sizeof *h + n);
    if (!h) return NULL;
    h->n = n; h->sid = tracking ? stack_id() : 0;
    charge(h, 1);
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
    charge(h, -1);
    free(h);
}
/* The device's realloc keeps the block (tlsf grows or shrinks in place when it
 * can) and so keeps its site: charged as free + malloc of the new length under
 * the ORIGINAL allocation's stack, which is the DynBuf or array that grew. */
static void *pk_realloc(void *o, void *p, size_t n) {
    if (!p) return pk_malloc(o, n);
    if (!n) { pk_free(o, p); return NULL; }
    hdr_t *h = (hdr_t *)p - 1;
    charge(h, -1);
    hdr_t *q = realloc(h, sizeof *q + n);
    if (!q) { charge(h, 1); return NULL; }
    q->n = n;
    charge(q, 1);
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
    if (argc < 3) { fprintf(stderr, "usage: compile_peak FILE.js OUTDIR [--read BC] [--write BC]\n"); return 3; }
    const char *read_bc = NULL, *write_bc = NULL;
    for (int i = 3; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--read")) read_bc = argv[i + 1];
        else if (!strcmp(argv[i], "--write")) write_bc = argv[i + 1];
    }
    char path[512];
    snprintf(path, sizeof path, "%s/series.tsv", argv[2]);
    series = fopen(path, "w");
    JSRuntime *rt = JS_NewRuntime2(&MF, NULL);
    JSContext *ctx = JS_NewContext(rt);
    JS_RunGC(rt);
    const size_t before = cur;
    size_t len;
    char *src = slurp(read_bc ? read_bc : argv[1], &len);
    peak = cur;
    tracking = 1;
    JSValue fn;
    if (read_bc) {
        fn = JS_ReadObject(ctx, (const uint8_t *)src, len, JS_READ_OBJ_BYTECODE);
    } else {
        fn = JS_Eval(ctx, src, len, argv[1], JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    }
    const size_t compile_peak = peak;
    tracking = 0;
    if (JS_IsException(fn)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        fprintf(stderr, "compile failed: %s\n", m ? m : "?");
        return 1;
    }
    JS_RunGC(rt);
    const size_t kept = cur;
    if (write_bc) {
        size_t n;
        uint8_t *buf = JS_WriteObject(ctx, &n, fn, JS_WRITE_OBJ_BYTECODE);
        FILE *f = fopen(write_bc, "wb");
        fwrite(buf, 1, n, f);
        fclose(f);
        js_free(ctx, buf);
        printf("wrote %s: %zu B\n", write_bc, n);
    }
    snprintf(path, sizeof path, "%s/peak.tsv", argv[2]);
    FILE *f = fopen(path, "w");
    for (int i = 0; i < nstacks; i++)
        if (snap[i]) fprintf(f, "%d\t%zu\t%zu\n", i, snap[i], snap_n[i]);
    fclose(f);
    snprintf(path, sizeof path, "%s/stacks.tsv", argv[2]);
    f = fopen(path, "w");
    for (int i = 0; i < nstacks; i++) {
        fprintf(f, "%d", i);
        for (int k = 0; k < stacks[i].depth; k++) fprintf(f, "\t%p", stacks[i].pc[k]);
        fprintf(f, "\n");
    }
    fclose(f);
    snprintf(path, sizeof path, "%s/summary.txt", argv[2]);
    f = fopen(path, "w");
    fprintf(f, "source %zu before %zu peak %zu (+%zu) kept %zu (+%zu) stacks %d events %zu\n",
            len, before, compile_peak, compile_peak - before, kept, kept - before, nstacks, events);
    fclose(f);
    printf("source %zu B: peak +%zu, kept +%zu (before %zu)\n", len, compile_peak - before,
           kept - before, before);
    if (series) fclose(series);
    JS_FreeValue(ctx, fn);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 0;
}
