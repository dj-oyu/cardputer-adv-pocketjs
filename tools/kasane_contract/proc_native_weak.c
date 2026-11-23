/* Weak stand-ins for kobako_shiki_frame.c's native line API, so a harness links
 * against a kobako_shiki_frame.c from before that API existed (the old
 * --proc-source baseline comparison of run_kobako_proc_qjs.py, gone with the
 * float VM in Shiki v2.0): kobako.shapes links and draws nothing. The tree's
 * kobako_shiki_frame.c defines the real ones, which the linker takes over these. */
#include "quickjs.h"
#include "kobako_api.h"
#include "kobako_shiki_frame.h"
#include <stdlib.h>

/* This minimal raster harness does not link kobako_api.c. Match its runtime
 * owner bookkeeping, including pointer reuse after the fixture's teardown.
 * A real kobako_api implementation overrides this weak stand-in. */
static kobako_class_registry_t proc_host_class_registry;
__attribute__((weak)) bool kobako_api_class_ready(JSRuntime *rt,kobako_class_owner_t *owner,JSClassID *id){
    return kobako_class_registry_ready(&proc_host_class_registry,rt,owner,id);
}
__attribute__((weak)) void kobako_api_class_forget(kobako_class_owner_t *owner){
    kobako_class_registry_forget(&proc_host_class_registry,owner);
}
void kobako_proc_host_classes_reset(void){
    kobako_class_registry_reset(&proc_host_class_registry);
}

__attribute__((weak)) JSValue kobako_proc_native_gouraud(JSContext *ctx,const char *op,const ksn_proc_gouraud_patch *patch){
    (void)patch;
    return kobako_api_throw(ctx,KOBAKO_ERR_UNSUPPORTED,op,"Gouraud patches unavailable in this harness",false,NULL);
}
__attribute__((weak)) JSValue kobako_proc_native_blend_region(JSContext *ctx,const char *op,const ksn_proc_blend_ellipsoids *patch,
        const ksn_proc_blend_region_mask *mask){
    (void)patch;(void)mask;
    return kobako_api_throw(ctx,KOBAKO_ERR_UNSUPPORTED,op,"blended regions unavailable in this harness",false,NULL);
}
__attribute__((weak)) JSValue kobako_proc_native_blend_ellipsoids(JSContext *ctx,const char *op,const ksn_proc_blend_ellipsoids *patch){
    (void)patch;
    return kobako_api_throw(ctx,KOBAKO_ERR_UNSUPPORTED,op,"blended surfaces unavailable in this harness",false,NULL);
}

__attribute__((weak)) JSValue kobako_proc_native_ellipsoid(JSContext *ctx,const char *op,const ksn_proc_ellipsoid *patch){
    (void)patch;
    return kobako_api_throw(ctx,KOBAKO_ERR_UNSUPPORTED,op,"analytic patches unavailable in this harness",false,NULL);
}
__attribute__((weak)) JSValue kobako_proc_native_clipped_ellipsoid(JSContext *ctx,const char *op,const ksn_proc_clipped_ellipsoid *patch){
    (void)patch;
    return kobako_api_throw(ctx,KOBAKO_ERR_UNSUPPORTED,op,"clipped patches unavailable in this harness",false,NULL);
}

__attribute__((weak)) JSValue kobako_proc_native_begin(JSContext *ctx,const char *op,kobako_proc_mark *mark){
    (void)ctx;(void)op;*mark=(kobako_proc_mark){0};return JS_UNDEFINED;
}
__attribute__((weak)) JSValue kobako_proc_native_line(JSContext *ctx,const char *op,int x0,int y0,int x1,int y1,
                                                      uint16_t color){
    (void)ctx;(void)op;(void)x0;(void)y0;(void)x1;(void)y1;(void)color;return JS_UNDEFINED;
}
__attribute__((weak)) void kobako_proc_native_rollback(const kobako_proc_mark *mark){(void)mark;}

__attribute__((weak)) bool kobako_proc_native_mesh_depth(void){return false;}
__attribute__((weak)) JSValue kobako_proc_native_mesh(JSContext *ctx,const char *op,const ksn_proc_gouraud_patch *p,const float *z,uint16_t node){
    (void)p;(void)z;(void)node;
    return kobako_api_throw(ctx,KOBAKO_ERR_UNSUPPORTED,op,"depth unavailable in this harness",false,NULL);
}

__attribute__((weak)) JSValue kobako_api_root(JSContext *ctx){
    JSValue global=JS_GetGlobalObject(ctx),root=JS_GetPropertyStr(ctx,global,"kobako");
    JS_FreeValue(ctx,global);return root;
}

__attribute__((weak)) JSValue kobako_proc_native_sided_mesh(JSContext *ctx,const char *op,const ksn_proc_gouraud_patch *p,
        const float *depth,const uint16_t *colors,const uint8_t bits[8],uint16_t node){
    (void)p;(void)depth;(void)colors;(void)bits;(void)node;
    return kobako_api_throw(ctx,KOBAKO_ERR_UNSUPPORTED,op,"sided mesh snapshot unavailable in this harness",false,NULL);
}
