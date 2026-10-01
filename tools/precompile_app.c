/* Build-only compiler. No native surfaces and no execution of app code.
 * Debug tables stay intact: source and bytecode errors must name the same file.
 * Output is experimental until the host/target serialization gate is proven. */
#include "quickjs.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

static int exception(JSContext *ctx) {
    JSValue e = JS_GetException(ctx);
    const char *s = JS_ToCString(ctx, e);
    fprintf(stderr, "%s\n", s ? s : "exception (unprintable)");
    if (s) JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, e);
    return 1;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--identity")) {
        uint16_t endian = 1;
        printf("{\"pointer_bytes\":%zu,\"jsvalue_bytes\":%zu,\"little_endian\":%s,"
               "\"eval_flags\":%d,\"write_flags\":%d}\n", sizeof(void *), sizeof(JSValue),
               *(uint8_t *)&endian ? "true" : "false",
               JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY, JS_WRITE_OBJ_BYTECODE);
        return 0;
    }
    if (argc != 4) {
        fprintf(stderr, "precompile_app SOURCE DISPLAY_FILENAME OUTPUT\n");
        return 2;
    }
    /* Module graphs need a chunk-table loader and an instance-identity test;
     * reject rather than silently compiling them as global scripts. */
    size_t fnlen = strlen(argv[2]);
    if (fnlen >= 4 && !strcmp(argv[2] + fnlen - 4, ".mjs")) {
        fprintf(stderr, "module precompile is not enabled\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    if (fseek(f, 0, SEEK_END)) { fclose(f); return 2; }
    long n = ftell(f);
    if (n < 0 || (uint64_t)n >= SIZE_MAX) { fclose(f); return 2; }
    rewind(f);
    char *source = malloc((size_t)n + 1);
    if (!source) { fclose(f); return 2; }
    if (fread(source, 1, (size_t)n, f) != (size_t)n) {
        free(source); fclose(f); return 2;
    }
    source[n] = 0;
    fclose(f);
    JSRuntime *rt = JS_NewRuntime();
    if (!rt) { free(source); return 1; }
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) { free(source); JS_FreeRuntime(rt); return 1; }
    JSValue fn = JS_Eval(ctx, source, (size_t)n, argv[2],
                         JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    free(source);
    int status = 1;
    if (JS_IsException(fn)) { exception(ctx); goto done; }
    size_t size = 0;
    uint8_t *buf = JS_WriteObject(ctx, &size, fn, JS_WRITE_OBJ_BYTECODE);
    JS_FreeValue(ctx, fn);
    if (!buf) { exception(ctx); goto done; }
    /* A round trip detects serializer/read regressions without running a
     * source's top-level side effects. It is not cross-architecture proof. */
    fn = JS_ReadObject(ctx, buf, size, JS_READ_OBJ_BYTECODE);
    if (JS_IsException(fn)) { exception(ctx); js_free(ctx, buf); goto done; }
    if (JS_VALUE_GET_TAG(fn) != JS_TAG_FUNCTION_BYTECODE) {
        JS_FreeValue(ctx, fn); js_free(ctx, buf); goto done;
    }
    JS_FreeValue(ctx, fn);
    f = fopen(argv[3], "wb");
    if (!f) { perror(argv[3]); js_free(ctx, buf); goto done; }
    status = fwrite(buf, 1, size, f) == size ? 0 : 2;
    if (fclose(f)) status = 2;
    js_free(ctx, buf);
done:
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return status;
}
