/* pocket_proc_native_*() for host harnesses without the real
 * main/pocket/pocket_proc.c: each segment pocket.shapes draws goes to the
 * script's globalThis.__shapeSeg(x0, y0, x1, y1, color). The MEGADEMO
 * harnesses define it as SHAPE_DRAW_JS (shape_draw_shim.h).
 * docs/api/shapes.md. */
#include "quickjs.h"
#include "pocket_proc.h"

JSValue pocket_proc_native_begin(JSContext *ctx,const char *op,pocket_proc_mark *mark){
    (void)ctx;(void)op;*mark=(pocket_proc_mark){0};return JS_UNDEFINED;
}
JSValue pocket_proc_native_line(JSContext *ctx,const char *op,int x0,int y0,int x1,int y1,uint16_t color){
    (void)op;
    JSValue g=JS_GetGlobalObject(ctx),f=JS_GetPropertyStr(ctx,g,"__shapeSeg");
    JSValue args[5]={JS_NewInt32(ctx,x0),JS_NewInt32(ctx,y0),JS_NewInt32(ctx,x1),JS_NewInt32(ctx,y1),
                     JS_NewInt32(ctx,color)};
    JSValue r=JS_Call(ctx,f,g,5,args);
    JS_FreeValue(ctx,f);JS_FreeValue(ctx,g);
    return r;
}
void pocket_proc_native_rollback(const pocket_proc_mark *mark){(void)mark;}
