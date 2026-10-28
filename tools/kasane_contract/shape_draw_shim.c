/* pocket_proc_draw_numbers() for host harnesses without the real
 * main/pocket/pocket_proc.c: pocket.shapes' draws go to the script's
 * globalThis.__shapeDraw(handle, inputs). The MEGADEMO harnesses define it
 * as SHAPE_DRAW_JS below: through the frame loop's procedural mock when WAKE
 * draws through emit, else into WAKE's list (plan k arrives as k + 1, as
 * megademo_wake.js's wh() sends it). docs/apps/megademo-shapes.md. */
#include "quickjs.h"
#include "pocket_proc.h"

JSValue pocket_proc_draw_numbers(JSContext *ctx,uint32_t handle,const double *in,unsigned n){
    JSValue g=JS_GetGlobalObject(ctx),f=JS_GetPropertyStr(ctx,g,"__shapeDraw"),a=JS_NewArray(ctx);
    for(unsigned i=0;i<n;i++)JS_SetPropertyUint32(ctx,a,i,JS_NewFloat64(ctx,in[i]));
    JSValue args[2]={JS_NewUint32(ctx,handle),a};
    JSValue r=JS_Call(ctx,f,g,2,args);
    JS_FreeValue(ctx,args[1]);JS_FreeValue(ctx,f);JS_FreeValue(ctx,g);
    return r;
}
