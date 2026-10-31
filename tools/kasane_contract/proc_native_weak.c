/* Weak stand-ins for pocket_proc.c's native line API, for
 * run_pocket_proc_qjs.py --proc-source with a pocket_proc.c from before it
 * existed (CI's baseline raster comparison, .github/workflows): pocket.shapes
 * links and draws nothing. The tree's pocket_proc.c defines the real ones,
 * which the linker takes over these. */
#include "quickjs.h"
#include "pocket_api.h"
#include "pocket_proc.h"
#include <stdlib.h>

/* This minimal raster harness does not link pocket_api.c. Match its runtime
 * owner bookkeeping, including pointer reuse after the fixture's teardown.
 * A real pocket_api implementation overrides this weak stand-in. */
static pocket_class_registry_t proc_host_class_registry;
__attribute__((weak)) bool pocket_api_class_ready(JSRuntime *rt,pocket_class_owner_t *owner,JSClassID *id){
    return pocket_class_registry_ready(&proc_host_class_registry,rt,owner,id);
}
__attribute__((weak)) void pocket_api_class_forget(pocket_class_owner_t *owner){
    pocket_class_registry_forget(&proc_host_class_registry,owner);
}
void pocket_proc_host_classes_reset(void){
    pocket_class_registry_reset(&proc_host_class_registry);
}

__attribute__((weak)) JSValue pocket_proc_native_blend_region(JSContext *ctx,const char *op,const ksn_proc_blend_ellipsoids *patch,
        const ksn_proc_blend_region_mask *mask){
    (void)patch;(void)mask;
    return pocket_api_throw(ctx,POCKET_ERR_UNSUPPORTED,op,"blended regions unavailable in this harness",false,NULL);
}
__attribute__((weak)) JSValue pocket_proc_native_blend_ellipsoids(JSContext *ctx,const char *op,const ksn_proc_blend_ellipsoids *patch){
    (void)patch;
    return pocket_api_throw(ctx,POCKET_ERR_UNSUPPORTED,op,"blended surfaces unavailable in this harness",false,NULL);
}

__attribute__((weak)) JSValue pocket_proc_native_ellipsoid(JSContext *ctx,const char *op,const ksn_proc_ellipsoid *patch){
    (void)patch;
    return pocket_api_throw(ctx,POCKET_ERR_UNSUPPORTED,op,"analytic patches unavailable in this harness",false,NULL);
}
__attribute__((weak)) JSValue pocket_proc_native_clipped_ellipsoid(JSContext *ctx,const char *op,const ksn_proc_clipped_ellipsoid *patch){
    (void)patch;
    return pocket_api_throw(ctx,POCKET_ERR_UNSUPPORTED,op,"clipped patches unavailable in this harness",false,NULL);
}

__attribute__((weak)) JSValue pocket_proc_native_begin(JSContext *ctx,const char *op,pocket_proc_mark *mark){
    (void)ctx;(void)op;*mark=(pocket_proc_mark){0};return JS_UNDEFINED;
}
__attribute__((weak)) JSValue pocket_proc_native_line(JSContext *ctx,const char *op,int x0,int y0,int x1,int y1,
                                                      uint16_t color){
    (void)ctx;(void)op;(void)x0;(void)y0;(void)x1;(void)y1;(void)color;return JS_UNDEFINED;
}
__attribute__((weak)) void pocket_proc_native_rollback(const pocket_proc_mark *mark){(void)mark;}
