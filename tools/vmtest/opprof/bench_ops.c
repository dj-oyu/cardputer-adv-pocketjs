/* Runs one script on the profiling QuickJS (opprof_impl.c; built by
 * run_derby_prof.py --bench): a runtime, a context (with __opprof), the
 * script, the job queue. Nothing of the firmware's surfaces. */
#include <stdio.h>
#include <stdlib.h>
#include "quickjs.h"

int main(int argc, char **argv)
{
    if (argc < 2) return 3;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 3;
    static char src[1 << 16];
    size_t n = fread(src, 1, sizeof src - 1, f);
    fclose(f);
    src[n] = 0;
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    JSValue v = JS_Eval(ctx, src, n, argv[1], JS_EVAL_TYPE_GLOBAL);
    int bad = JS_IsException(v);
    if (bad) {
        JSValue e = JS_GetException(ctx);
        const char *s = JS_ToCString(ctx, e);
        fprintf(stderr, "exception: %s\n", s ? s : "?");
    }
    JS_FreeValue(ctx, v);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return bad;
}
