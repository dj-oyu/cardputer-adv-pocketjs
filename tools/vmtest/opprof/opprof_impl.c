/* Bytecode profiler for the host QuickJS (docs/apps/derby-wide2-profile.md).
 *
 * Never part of the firmware or of any normal host build: patch_quickjs.py
 * writes a copy of quickjs.c that #includes this file at its end and calls
 * the opprof_* hooks from the dispatch macro, the two C-call entries,
 * js_malloc_rt / js_calloc_rt / js_free_rt / js_realloc_rt and
 * js_trigger_gc. It lives inside quickjs.c's translation unit because it
 * reads private structures (JSStackFrame, JSFunctionBytecode, JSObject).
 *
 * The guest controls it through one global, __opprof (added by the
 * JS_NewContext wrapper below; link with -Wl,--wrap=JS_NewContext):
 *   __opprof(1)        start counting
 *   __opprof(0, tag)   stop, append this window's counts to $OPPROF_OUT
 *                      under tag, and clear them
 *
 * Per window and per bytecode function: dispatches by opcode, self (the
 * function executing it) and inclusive (every function on the frame chain,
 * once each); of those, the arithmetic and comparison dispatches with a
 * float64 operand (the device does double in software); array literals
 * (OP_array_from) by length; function entries; calls into C functions by
 * callee; guest-heap allocations. Natives and allocations go to the
 * innermost visible bytecode function (self) and to every visible one on
 * the chain (inclusive).
 *
 * OPPROF_HIDE=a.js,b.js names the harness's own scripts (its wrappers
 * around the native API): their functions are invisible. Their dispatches,
 * their allocations, and their calls into C are counted apart (HIDDEN),
 * except calls into the natives OPPROF_KEEP names (the device API the
 * wrapper stands in front of, e.g. draw): those count as the app's, from
 * the nearest visible caller. */
#include <stdio.h>

int opprof_on;
static FILE *opprof_out;
static JSRuntime *opprof_rt;

#define OPPROF_MAXF 1024
#define OPPROF_MAXN 128          /* natives (slots) */
#define OPPROF_ARR 65            /* array literal lengths, 64 = 64 or more */
typedef struct {
    const void *key;             /* JSFunctionBytecode * or the C function's JSObject * */
    int native, hidden, keep, slot;
    char name[96];
    uint64_t calls, allocs, alloc_bytes, reallocs, frees, incl_allocs;
    uint32_t stamp;
    JSFunctionBytecode *b;    /* bytecode functions: for the line table at dump time */
    uint32_t *pcs;            /* self dispatches by bytecode offset (lazily allocated) */
} OpprofFn;
static OpprofFn opprof_fn[OPPROF_MAXF];
static int opprof_nfn, opprof_nslot;
static int opprof_slot_fn[OPPROF_MAXN];
static int opprof_hash[4096];         /* open addressing, index + 1 */
static uint64_t opprof_self[OPPROF_MAXF][256], opprof_sfop[OPPROF_MAXF][256];
static uint64_t opprof_incl[OPPROF_MAXF][256], opprof_ifop[OPPROF_MAXF][256];
static uint64_t opprof_sarr[OPPROF_MAXF][OPPROF_ARR], opprof_iarr[OPPROF_MAXF][OPPROF_ARR];
static uint64_t opprof_snat[OPPROF_MAXF][OPPROF_MAXN], opprof_inat[OPPROF_MAXF][OPPROF_MAXN];
static uint64_t opprof_gc, opprof_hidden_ops, opprof_hidden_nat, opprof_hidden_alloc;
static uint32_t opprof_stamp;
#define OPPROF_MAXE 4096
static struct { int caller, callee; uint64_t n; } opprof_edge[OPPROF_MAXE];   /* entries by caller */
static int opprof_nedge;

static const char *const opprof_opname[256] = {
#define FMT(f)
#define DEF(id, size, n_pop, n_push, f) #id,
#define def(id, size, n_pop, n_push, f)
#include "quickjs-opcode.h"
#undef def
#undef DEF
#undef FMT
};

static bool opprof_listed(const char *env, const char *s, size_t n)
{
    const char *l = getenv(env);
    for (const char *h = l; h && *h; h = strchr(h, ',') ? strchr(h, ',') + 1 : NULL)
        if (!strncmp(h, s, n) && (h[n] == ',' || !h[n]))
            return true;
    return false;
}

static int opprof_find(const void *key)
{
    unsigned h = ((uintptr_t)key >> 3) * 2654435761u >> 20 & 4095;
    while (opprof_hash[h]) {
        if (opprof_fn[opprof_hash[h] - 1].key == key)
            return opprof_hash[h] - 1;
        h = (h + 1) & 4095;
    }
    if (opprof_nfn >= OPPROF_MAXF) {
        fprintf(stderr, "opprof: too many functions\n");
        abort();
    }
    opprof_fn[opprof_nfn].key = key;
    opprof_fn[opprof_nfn].slot = -1;
    opprof_hash[h] = opprof_nfn + 1;
    return opprof_nfn++;
}

static int opprof_bc(JSRuntime *rt, JSFunctionBytecode *b)
{
    int i = opprof_find(b);
    OpprofFn *f = &opprof_fn[i];
    if (!f->name[0]) {
        char n[64], file[256];
        const char *fn = JS_AtomGetStrRT(rt, n, sizeof n, b->func_name);
        const char *fl = JS_AtomGetStrRT(rt, file, sizeof file, b->filename);
        const char *base = strrchr(fl, '/'), *bn = base ? base + 1 : fl;
        snprintf(f->name, sizeof f->name, "%s@%s:%d:%d", fn[0] ? fn : "<anon>", bn, b->line_num, b->col_num);
        f->hidden = opprof_listed("OPPROF_HIDE", bn, strlen(bn));
        f->b = b;
    }
    return i;
}

/* The bytecode function of frame fr, or -1 (a C function's frame). */
static int opprof_frame_bc(JSRuntime *rt, JSStackFrame *fr)
{
    if (JS_VALUE_GET_TAG(fr->cur_func) != JS_TAG_OBJECT)
        return -1;
    JSObject *p = JS_VALUE_GET_OBJ(fr->cur_func);
    if (p->class_id != JS_CLASS_BYTECODE_FUNCTION)
        return -1;
    return opprof_bc(rt, p->u.func.function_bytecode);
}

/* Calls fn(k) once for every visible bytecode function on the chain from
 * fr; returns the innermost bytecode function (visible or not), or -1. */
#define OPPROF_CHAIN(fr, body) do { \
    ++opprof_stamp; \
    for (JSStackFrame *opprof_w = (fr); opprof_w; opprof_w = opprof_w->prev_frame) { \
        int k = opprof_frame_bc(opprof_rt, opprof_w); \
        if (k >= 0 && !opprof_fn[k].hidden && opprof_fn[k].stamp != opprof_stamp) { \
            opprof_fn[k].stamp = opprof_stamp; \
            body; \
        } \
    } \
} while (0)

static int opprof_innermost(JSStackFrame *fr, int *visible)
{
    int inner = -1;
    *visible = -1;
    for (; fr; fr = fr->prev_frame) {
        int k = opprof_frame_bc(opprof_rt, fr);
        if (k < 0) continue;
        if (inner < 0) inner = k;
        if (!opprof_fn[k].hidden) { *visible = k; break; }
    }
    return inner;
}

static void opprof_hit(JSRuntime *rt, JSFunctionBytecode *b, JSStackFrame *sf, const uint8_t *pc, JSValue *sp)
{
    int op = *pc, i = opprof_bc(rt, b), fl = 0, arr = -1;
    OpprofFn *f = &opprof_fn[i];
    opprof_rt = rt;
    if (f->hidden) {
        opprof_hidden_ops++;
        return;
    }
    if (pc == b->byte_code_buf) {
        /* An entry: one more on the edge from the nearest visible caller. */
        int c = -1, e;
        f->calls++;
        for (JSStackFrame *fr = sf->prev_frame; fr && c < 0; fr = fr->prev_frame) {
            int k = opprof_frame_bc(rt, fr);
            if (k >= 0 && !opprof_fn[k].hidden) c = k;
        }
        for (e = 0; e < opprof_nedge; e++)
            if (opprof_edge[e].caller == c && opprof_edge[e].callee == i) break;
        if (e == opprof_nedge && opprof_nedge < OPPROF_MAXE) {
            opprof_edge[e].caller = c;
            opprof_edge[e].callee = i;
            opprof_edge[e].n = 0;
            opprof_nedge++;
        }
        if (e < OPPROF_MAXE) opprof_edge[e].n++;
    }
    if (!f->pcs) f->pcs = calloc(b->byte_code_len, sizeof *f->pcs);
    f->pcs[pc - b->byte_code_buf]++;
    switch (op) {
    case OP_add: case OP_sub: case OP_mul: case OP_div: case OP_mod: case OP_lt:
    case OP_lte: case OP_gt: case OP_gte: case OP_strict_eq: case OP_strict_neq:
    case OP_eq: case OP_neq: case OP_shl: case OP_sar: case OP_shr: case OP_and:
    case OP_or: case OP_xor:
        fl = JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(sp[-1])) || JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(sp[-2]));
        break;
    case OP_neg: case OP_inc: case OP_dec: case OP_post_inc: case OP_post_dec: case OP_plus:
        fl = JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(sp[-1]));
        break;
    case OP_array_from: {
        unsigned n = get_u16(pc + 1);
        arr = n < OPPROF_ARR - 1 ? n : OPPROF_ARR - 1;
        break;
    }
    }
    opprof_self[i][op]++;
    if (fl) opprof_sfop[i][op]++;
    if (arr >= 0) opprof_sarr[i][arr]++;
    OPPROF_CHAIN(sf, {
        opprof_incl[k][op]++;
        if (fl) opprof_ifop[k][op]++;
        if (arr >= 0) opprof_iarr[k][arr]++;
    });
}

static void opprof_ccall(JSContext *ctx, JSValueConst func_obj)
{
    JSObject *p = JS_VALUE_GET_OBJ(func_obj);
    int i = opprof_find(p);
    OpprofFn *f = &opprof_fn[i];
    if (!f->name[0]) {
        int on = opprof_on;
        opprof_on = 0;   /* the name lookup is not the guest's */
        JSValue v = JS_GetProperty(ctx, func_obj, JS_ATOM_name);
        const char *s = JS_IsString(v) ? JS_ToCString(ctx, v) : NULL;
        snprintf(f->name, sizeof f->name, "native:%s", s && s[0] ? s : "?");
        f->native = 1;
        f->keep = s && opprof_listed("OPPROF_KEEP", s, strlen(s));
        if (s) JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, v);
        opprof_on = on;
    }
    if (f->slot < 0) {
        if (opprof_nslot >= OPPROF_MAXN) { fprintf(stderr, "opprof: too many natives\n"); abort(); }
        opprof_slot_fn[opprof_nslot] = i;
        f->slot = opprof_nslot++;
    }
    if (!opprof_rt) return;
    int vis, inner = opprof_innermost(opprof_rt->current_stack_frame, &vis);
    if (inner >= 0 && opprof_fn[inner].hidden && !f->keep) {
        opprof_hidden_nat++;
        return;
    }
    f->calls++;
    if (vis >= 0) opprof_snat[vis][f->slot]++;
    int s = f->slot;
    OPPROF_CHAIN(opprof_rt->current_stack_frame, opprof_inat[k][s]++);
}

static void opprof_alloc(int kind, size_t size)
{
    if (!opprof_rt) return;
    int vis, inner = opprof_innermost(opprof_rt->current_stack_frame, &vis);
    if (inner < 0 || opprof_fn[inner].hidden) {
        if (kind == 0) opprof_hidden_alloc++;
        return;
    }
    OpprofFn *f = &opprof_fn[vis];
    if (kind == 0) {
        f->allocs++;
        f->alloc_bytes += size;
        OPPROF_CHAIN(opprof_rt->current_stack_frame, opprof_fn[k].incl_allocs++);
    } else if (kind == 1) f->reallocs++;
    else f->frees++;
}

static void opprof_gc_hit(void) { opprof_gc++; }

static void opprof_row(FILE *o, const char *tag, int i, const uint64_t *v, int n, bool names)
{
    bool any = false;
    for (int k = 0; k < n; k++) any |= v[k] != 0;
    if (!any) return;
    fprintf(o, "%s %d", tag, i);
    for (int k = 0; k < n; k++)
        if (v[k]) {
            if (names) fprintf(o, " %s=%llu", opprof_opname[k] ? opprof_opname[k] : "?", (unsigned long long)v[k]);
            else fprintf(o, " %d=%llu", k, (unsigned long long)v[k]);
        }
    fputc('\n', o);
}

static void opprof_dump(JSContext *ctx, const char *tag)
{
    if (!opprof_out) {
        const char *p = getenv("OPPROF_OUT");
        opprof_out = fopen(p ? p : "opprof.txt", "w");
        if (!opprof_out) abort();
    }
    FILE *o = opprof_out;
    fprintf(o, "FRAME %s\nGC %llu\nHIDDEN %llu %llu %llu\n", tag, (unsigned long long)opprof_gc,
            (unsigned long long)opprof_hidden_ops, (unsigned long long)opprof_hidden_nat,
            (unsigned long long)opprof_hidden_alloc);
    for (int i = 0; i < opprof_nfn; i++) {
        OpprofFn *f = &opprof_fn[i];
        uint64_t s = 0, in = 0;
        for (int op = 0; op < 256; op++) s += opprof_self[i][op], in += opprof_incl[i][op];
        if (!s && !in && !f->calls && !f->allocs && !f->frees) continue;
        fprintf(o, "FN %d %d %d %llu %llu %llu %llu %llu %llu %llu %llu %s\n", i, f->native, f->hidden,
                (unsigned long long)f->calls, (unsigned long long)s, (unsigned long long)in,
                (unsigned long long)f->allocs, (unsigned long long)f->alloc_bytes,
                (unsigned long long)f->reallocs, (unsigned long long)f->frees,
                (unsigned long long)f->incl_allocs, f->name);
        opprof_row(o, "OPS", i, opprof_self[i], 256, true);
        opprof_row(o, "SFOP", i, opprof_sfop[i], 256, true);
        opprof_row(o, "IOPS", i, opprof_incl[i], 256, true);
        opprof_row(o, "IFOP", i, opprof_ifop[i], 256, true);
        opprof_row(o, "SARR", i, opprof_sarr[i], OPPROF_ARR, false);
        opprof_row(o, "IARR", i, opprof_iarr[i], OPPROF_ARR, false);
        /* natives by function index, not slot */
        bool any = false;
        for (int k = 0; k < opprof_nslot; k++) any |= opprof_snat[i][k] || opprof_inat[i][k];
        if (any) {
            fprintf(o, "NAT %d", i);
            for (int k = 0; k < opprof_nslot; k++)
                if (opprof_snat[i][k] || opprof_inat[i][k])
                    fprintf(o, " %d=%llu/%llu", opprof_slot_fn[k], (unsigned long long)opprof_snat[i][k],
                            (unsigned long long)opprof_inat[i][k]);
            fputc('\n', o);
        }
        if (f->pcs) {
            /* Self dispatches by source line (line=count). */
            int lines[512], nl = 0;
            uint64_t cnt[512];
            for (int pcv = 0; pcv < f->b->byte_code_len; pcv++) {
                if (!f->pcs[pcv]) continue;
                int col, ln = find_line_num(ctx, f->b, pcv, &col), j;
                for (j = 0; j < nl && lines[j] != ln; j++) {}
                if (j == nl && nl < 512) { lines[nl] = ln; cnt[nl++] = 0; }
                if (j < nl) cnt[j] += f->pcs[pcv];
                f->pcs[pcv] = 0;
            }
            if (nl) {
                fprintf(o, "LINES %d", i);
                for (int j = 0; j < nl; j++) fprintf(o, " %d=%llu", lines[j], (unsigned long long)cnt[j]);
                fputc('\n', o);
            }
        }
    }
    for (int e = 0; e < opprof_nedge; e++)
        fprintf(o, "EDGE %d %d %llu\n", opprof_edge[e].caller, opprof_edge[e].callee,
                (unsigned long long)opprof_edge[e].n);
    opprof_nedge = 0;
    fprintf(o, "END\n");
    fflush(o);
    for (int i = 0; i < opprof_nfn; i++) {
        OpprofFn *f = &opprof_fn[i];
        f->calls = f->allocs = f->alloc_bytes = f->reallocs = f->frees = f->incl_allocs = 0;
    }
    memset(opprof_self, 0, sizeof opprof_self);
    memset(opprof_sfop, 0, sizeof opprof_sfop);
    memset(opprof_incl, 0, sizeof opprof_incl);
    memset(opprof_ifop, 0, sizeof opprof_ifop);
    memset(opprof_sarr, 0, sizeof opprof_sarr);
    memset(opprof_iarr, 0, sizeof opprof_iarr);
    memset(opprof_snat, 0, sizeof opprof_snat);
    memset(opprof_inat, 0, sizeof opprof_inat);
    opprof_gc = opprof_hidden_ops = opprof_hidden_nat = opprof_hidden_alloc = 0;
}

static JSValue js_opprof(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    int32_t on = 0;
    if (argc > 0) JS_ToInt32(ctx, &on, argv[0]);
    if (on) {
        opprof_rt = JS_GetRuntime(ctx);
        opprof_on = 1;
    } else {
        opprof_on = 0;
        const char *tag = argc > 1 ? JS_ToCString(ctx, argv[1]) : NULL;
        opprof_dump(ctx, tag ? tag : "-");
        if (tag) JS_FreeCString(ctx, tag);
    }
    return JS_UNDEFINED;
}

JSContext *__real_JS_NewContext(JSRuntime *rt);
JSContext *__wrap_JS_NewContext(JSRuntime *rt)
{
    JSContext *ctx = __real_JS_NewContext(rt);
    if (ctx) {
        JSValue g = JS_GetGlobalObject(ctx);
        JS_SetPropertyStr(ctx, g, "__opprof", JS_NewCFunction(ctx, js_opprof, "__opprof", 2));
        JS_FreeValue(ctx, g);
    }
    return ctx;
}
