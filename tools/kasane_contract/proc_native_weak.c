/* Weak stand-ins for pocket_proc.c's native line API, for
 * run_pocket_proc_qjs.py --proc-source with a pocket_proc.c from before it
 * existed (CI's baseline raster comparison, .github/workflows): pocket.shapes
 * links and draws nothing. The tree's pocket_proc.c defines the real ones,
 * which the linker takes over these. */
#include "quickjs.h"
#include "pocket_proc.h"

__attribute__((weak)) JSValue pocket_proc_native_begin(JSContext *ctx,const char *op,pocket_proc_mark *mark){
    (void)ctx;(void)op;*mark=(pocket_proc_mark){0};return JS_UNDEFINED;
}
__attribute__((weak)) JSValue pocket_proc_native_line(JSContext *ctx,const char *op,int x0,int y0,int x1,int y1,
                                                      uint16_t color){
    (void)ctx;(void)op;(void)x0;(void)y0;(void)x1;(void)y1;(void)color;return JS_UNDEFINED;
}
__attribute__((weak)) void pocket_proc_native_rollback(const pocket_proc_mark *mark){(void)mark;}
