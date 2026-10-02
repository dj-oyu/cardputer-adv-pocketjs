/* Serialize one file without evaluating it; a differential parser oracle. */
#include "quickjs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    FILE *in = fopen(argv[1], "rb");
    if (!in || fseek(in, 0, SEEK_END)) return 2;
    long n = ftell(in);
    if (n < 0 || fseek(in, 0, SEEK_SET)) return 2;
    char *source = malloc((size_t)n + 1);
    if (!source || fread(source, 1, (size_t)n, in) != (size_t)n) return 2;
    fclose(in); source[n] = 0;
    JSRuntime *rt = JS_NewRuntime();
    if (!rt) return 2;
    JS_SetMaxStackSize(rt, 0);
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) { JS_FreeRuntime(rt); free(source); return 2; }
    int flags = JS_EVAL_FLAG_COMPILE_ONLY;
    size_t name_len = strlen(argv[1]);
    const char *line_end = strchr(source, '\n');
    const char *module_flag = strstr(source, "--module");
    if ((name_len > 4 && !strcmp(argv[1] + name_len - 4, ".mjs")) ||
        (!strncmp(source, "// vmrun-flags:", 15) && module_flag &&
         (!line_end || module_flag < line_end))) flags |= JS_EVAL_TYPE_MODULE;
    JSValue fn = JS_Eval(ctx, source, (size_t)n, argv[1], flags);
    free(source);
    int rc = 0;
    if (JS_IsException(fn)) {
        JSValue error = JS_GetException(ctx);
        const char *text = JS_ToCString(ctx, error);
        fprintf(stderr, "%s\n", text ? text : "exception without text");
        JS_FreeCString(ctx, text); JS_FreeValue(ctx, error); rc = 1;
    } else {
        size_t len;
        uint8_t *bytes = JS_WriteObject(ctx, &len, fn, JS_WRITE_OBJ_BYTECODE);
        if (!bytes) rc = 2;
        else {
            FILE *out = fopen(argv[2], "wb");
            if (!out || fwrite(bytes, 1, len, out) != len) rc = 2;
            if (out) fclose(out);
            js_free(ctx, bytes);
        }
    }
    JS_FreeValue(ctx, fn); JS_FreeContext(ctx); JS_FreeRuntime(rt);
    return rc;
}
