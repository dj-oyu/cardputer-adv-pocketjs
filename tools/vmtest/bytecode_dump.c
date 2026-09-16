// Compile only: inspect pass-2 and final instructions without running a test.
#include <stdio.h>
#include <stdlib.h>
#include "quickjs.h"

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    FILE *file = fopen(argv[1], "rb");
    if (!file) return 2;
    if (fseek(file, 0, SEEK_END)) return 2;
    long length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET)) return 2;
    char *source = malloc((size_t)length + 1);
    if (!source || fread(source, 1, (size_t)length, file) != (size_t)length) return 2;
    fclose(file);
    source[length] = 0;
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    JS_SetDumpFlags(rt, JS_DUMP_BYTECODE_PASS2 | JS_DUMP_BYTECODE_FINAL);
    JSValue code = JS_Eval(ctx, source, length, argv[1], JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    int failed = JS_IsException(code);
    if (failed) {
        JSValue error = JS_GetException(ctx);
        const char *message = JS_ToCString(ctx, error);
        fprintf(stderr, "%s\n", message ? message : "compile error");
        JS_FreeCString(ctx, message);
        JS_FreeValue(ctx, error);
    }
    JS_FreeValue(ctx, code);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    free(source);
    return failed;
}
