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
static JSRuntime **proc_host_class_owners[16];
static unsigned proc_host_class_owner_count;
__attribute__((weak)) bool pocket_api_class_ready(JSRuntime *rt,JSRuntime **owner,JSClassID *id){
    if(*owner==rt)return true;
    *owner=rt;*id=0;
    for(unsigned i=0;i<proc_host_class_owner_count;++i)
        if(proc_host_class_owners[i]==owner)return false;
    if(proc_host_class_owner_count==sizeof proc_host_class_owners/sizeof proc_host_class_owners[0])abort();
    proc_host_class_owners[proc_host_class_owner_count++]=owner;
    return false;
}
void pocket_proc_host_classes_reset(void){
    for(unsigned i=0;i<proc_host_class_owner_count;++i)*proc_host_class_owners[i]=NULL;
    proc_host_class_owner_count=0;
}

__attribute__((weak)) JSValue pocket_proc_native_blend_ellipsoids(JSContext *ctx,const char *op,const ksn_proc_blend_ellipsoids *patch){
    (void)patch;
    return pocket_api_throw(ctx,POCKET_ERR_UNSUPPORTED,op,"blended surfaces unavailable in this harness",false,NULL);
}

__attribute__((weak)) JSValue pocket_proc_native_ellipsoid(JSContext *ctx,const char *op,const ksn_proc_ellipsoid *patch){
    (void)patch;
    return pocket_api_throw(ctx,POCKET_ERR_UNSUPPORTED,op,"analytic patches unavailable in this harness",false,NULL);
}

__attribute__((weak)) JSValue pocket_proc_native_begin(JSContext *ctx,const char *op,pocket_proc_mark *mark){
    (void)ctx;(void)op;*mark=(pocket_proc_mark){0};return JS_UNDEFINED;
}
__attribute__((weak)) JSValue pocket_proc_native_line(JSContext *ctx,const char *op,int x0,int y0,int x1,int y1,
                                                      uint16_t color){
    (void)ctx;(void)op;(void)x0;(void)y0;(void)x1;(void)y1;(void)color;return JS_UNDEFINED;
}
__attribute__((weak)) void pocket_proc_native_rollback(const pocket_proc_mark *mark){(void)mark;}
