/* Reuse the actual QuickJS skin lifecycle suite, replacing only the app's pose
 * source with production AyatsuriMotion. The default suite remains unchanged. */
#include "quickjs.h"
#include "ayatsuri/ayatsuri_model.h"
static void motion_setup(JSContext *ctx);
static void motion_pause(JSRuntime *,JSContext *,ayatsuri_model_instance *,const void *);
#define SKIN_JS_SETUP(ctx) motion_setup(ctx)
#define SKIN_JS_POST_WARM(rt,ctx,actor,owner) motion_pause(rt,ctx,actor,owner)
#include "test_js.c"
static void motion_file(JSContext *ctx,const char *path){
    FILE *f=fopen(path,"rb");CHECK(f);char text[16384];
    size_t n=fread(text,1,sizeof text-1,f);CHECK(feof(f)&&fclose(f)==0);text[n]=0;eval(ctx,text);
}
static void motion_setup(JSContext *ctx){
    motion_file(ctx,"apps/lib/ayatsuri_motion.js");
    motion_file(ctx,"tools/ayatsuri/skin/motion_scenario.js");
}
static void motion_pause(JSRuntime *rt,JSContext *ctx,ayatsuri_model_instance *actor,const void *owner){
    (void)rt;JSValue evaluate=function(ctx,"evaluateOnly"),draw=function(ctx,"drawA");
    call(ctx,evaluate,JS_NewInt32(ctx,30));oracle(ctx,actor,owner);
    size_t allocations=0;
    for(unsigned pause=0;pause<24;pause++){
        size_t before=alloc_calls;call(ctx,evaluate,JS_NewInt32(ctx,30));allocations+=alloc_calls-before;
        call(ctx,draw,JS_UNDEFINED);CHECK(present()==KSN_OK&&!memcmp(panel,reference,sizeof panel));
    }
    CHECK(!allocations);eval(ctx,"invalidMotion()");
    JS_FreeValue(ctx,evaluate);JS_FreeValue(ctx,draw);
    puts("MOTION_SKIN_PASS explicit_ms analytic_channels borrowed_output pause24 invalid_time_atomic warm_pause_alloc0");
}
