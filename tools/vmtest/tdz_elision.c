/* Which TDZ checks the compiler leaves in corpus/tdz_elision.js's functions
 * (tdz_elide_checks in quickjs.c, docs/vm/tdz-elision.md).
 *
 * corpus/tdz_elision.js pins the semantics (run.sh diffs it against node's
 * output); this pins the bytecode: for each top-level function, how many
 * get_loc_check / put_loc_check / set_loc_uninitialized remain in the final
 * (short-opcode) bytecode. The "throws" functions must keep a check on the
 * path that throws; the "elided" ones must have lost theirs. Built with
 * quickjs.c included, to read JSFunctionBytecode:
 *
 *   bash tools/vmtest/tdz_elision.sh            # build + run (WSL)
 *   bash tools/vmtest/tdz_elision.sh --print    # counts only, no verdict
 *
 * Build against an unmodified engine (VMTEST_QJS) with --print to see the
 * counts before the elision. */
#include "quickjs.c"

typedef struct { const char *name; int get_check, put_check, uninit; } Want;

/* Counted from the build and checked by hand against the source (the
 * comments say why each one stays); a change means the analysis proves more
 * or less than it did. */
static const Want WANT[] = {
    /* ---- the check on the throwing path stays */
    {"readBefore", 1, 0, 1},        /* the read before init keeps its check and x's kill; the read after loses its check */
    {"selfInit", 1, 0, 1},
    {"closureBefore", 0, 0, 1},     /* the throwing read is in the arrow; x is captured, so its kill stays */
    {"hoistedBefore", 0, 0, 1},
    {"switchCase", 1, 0, 1},        /* case 1 reads x past case 0's init */
    {"loopStale", 1, 0, 1},         /* y read before init on the 2nd iteration */
    {"loopStaleBlock", 3, 0, 3},    /* z in the try; out in the catch; j after the try/catch join */
    {"labeledBreak", 1, 0, 1},
    {"catchBefore", 1, 0, 1},       /* the catch handler knows nothing */
    {"finallyBefore", 1, 0, 1},
    {"typeofTdz", 1, 0, 1},
    {"classBefore", 1, 0, 2},
    {"destructDefault", 1, 0, 1},
    {"assignBefore", 0, 1, 1},      /* the store before init is put_loc_check */
    {"deadPathRead", 1, 0, 1},
    {"continueSkipsInit", 1, 0, 1},
    {"classHeritage", 1, 0, 2},
    {"classKey", 1, 0, 2},
    {"withTdz", 1, 0, 2},
    /* ---- the checks go, and the kills of uncaptured locals */
    {"straight", 0, 0, 0},
    {"joinBranches", 0, 0, 0},
    {"loopBody", 0, 0, 0},
    {"whileBreak", 0, 0, 0},
    {"forOf", 0, 0, 0},
    {"capturedLocal", 0, 0, 1},     /* n captured: its kill stays, its local reads go */
    {"freshPerIteration", 0, 0, 0},
    {"catchRead", 1, 0, 1},         /* a read in a catch handler keeps its check, and a's kill */
    {"labeledContinue", 0, 0, 0},
    {"closuresPerIteration", 0, 0, 2}, /* i and k captured */
    {"counters", 0, 0, 0},
    {"finallyAfter", 2, 0, 1},      /* r in the finally body and after the gosub */
    {"withAfter", 0, 0, 2},         /* inside with: the kills stay */
    {"switchInLoop", 0, 0, 0},
};

static void count(JSFunctionBytecode *b, int *g, int *p, int *u)
{
    int pos = 0, op;
    *g = *p = *u = 0;
    while (pos < b->byte_code_len) {
        op = b->byte_code_buf[pos];
        *g += op == OP_get_loc_check;
        *p += op == OP_put_loc_check;
        *u += op == OP_set_loc_uninitialized;
        pos += short_opcode_info(op).size;
    }
}

static JSValue js_print(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_UNDEFINED;
}

int main(int argc, char **argv)
{
    int print_only = argc > 2 && !strcmp(argv[2], "--print");
    if (argc < 2) return 3;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 3;
    static char src[1 << 16];
    size_t n = fread(src, 1, sizeof src - 1, f);
    fclose(f);
    src[n] = 0;
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "print", JS_NewCFunction(ctx, js_print, "print", 1));
    JSValue v = JS_Eval(ctx, src, n, argv[1], JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        fprintf(stderr, "eval failed\n");
        return 1;
    }
    JS_FreeValue(ctx, v);
    int fail = 0;
    for (size_t i = 0; i < sizeof WANT / sizeof WANT[0]; i++) {
        const Want *w = &WANT[i];
        JSValue fn = JS_GetPropertyStr(ctx, global, w->name);
        if (!JS_IsFunction(ctx, fn)) {
            printf("FAIL %s: not a function\n", w->name);
            fail = 1;
            JS_FreeValue(ctx, fn);
            continue;
        }
        JSFunctionBytecode *b = JS_VALUE_GET_OBJ(fn)->u.func.function_bytecode;
        int g, p, u;
        count(b, &g, &p, &u);
        if (print_only) {
            printf("%-22s get_loc_check=%d put_loc_check=%d set_loc_uninitialized=%d\n", w->name, g, p, u);
        } else if (g != w->get_check || p != w->put_check || u != w->uninit) {
            printf("FAIL %s: get_loc_check=%d put_loc_check=%d set_loc_uninitialized=%d, want %d %d %d\n",
                   w->name, g, p, u, w->get_check, w->put_check, w->uninit);
            fail = 1;
        } else {
            printf("PASS %s\n", w->name);
        }
        JS_FreeValue(ctx, fn);
    }
    JS_FreeValue(ctx, global);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return fail;
}
