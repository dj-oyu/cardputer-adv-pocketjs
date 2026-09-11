#include "pocket_random.h"
#include "pocket_api.h"
#include "random_stream.h"
#include <math.h>

static JSClassID stream_class;
static JSRuntime *stream_rt;
static void stream_finalize(JSRuntime *rt,JSValue val) {
    js_free_rt(rt,JS_GetOpaque(val,stream_class));
}
static const JSClassDef stream_def={
    .class_name="PocketRandomStream",.finalizer=stream_finalize,
};
static JSValue js_next(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv) {
    (void)argc;(void)argv;
    pocket_random_t *rng=JS_GetOpaque2(ctx,self,stream_class);
    if(!rng)return JS_EXCEPTION;
    return JS_NewUint32(ctx,pocket_random_next(rng));
}
static JSValue js_float(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv) {
    (void)argc;(void)argv;
    pocket_random_t *rng=JS_GetOpaque2(ctx,self,stream_class);
    if(!rng)return JS_EXCEPTION;
    return JS_NewFloat64(ctx,pocket_random_float(rng));
}
static JSValue js_seed(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv) {
    (void)self;(void)argc;(void)argv;
    return JS_NewUint32(ctx,pocket_random_seed());
}
static JSValue js_create(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv) {
    (void)self;
    double seed;
    if(argc<1||!JS_IsNumber(argv[0])||JS_ToFloat64(ctx,&seed,argv[0])<0||
       !isfinite(seed)||seed<0||seed>4294967295.0||floor(seed)!=seed)
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,"random.create",
                                "seed must be a uint32 number",false,NULL);
    JSValue obj=JS_NewObjectClass(ctx,stream_class);
    if(JS_IsException(obj))return obj;
    pocket_random_t *rng=js_malloc(ctx,sizeof(*rng));
    if(!rng) {JS_FreeValue(ctx,obj);return JS_EXCEPTION;}
    pocket_random_init(rng,(uint32_t)seed);
    JS_SetOpaque(obj,rng);
    return obj;
}
static const JSCFunctionListEntry methods[]={
    JS_CFUNC_DEF("nextUint32",0,js_next),
    JS_CFUNC_DEF("nextFloat",0,js_float),
};
static const JSCFunctionListEntry functions[]={
    JS_CFUNC_DEF("seed",0,js_seed),
    JS_CFUNC_DEF("create",1,js_create),
};
static esp_err_t build_random(JSContext *ctx,JSValueConst ns,void *user) {
    (void)user;
    JSRuntime *rt=JS_GetRuntime(ctx);
    if(stream_rt!=rt) {
        stream_class=0;
        JS_NewClassID(rt,&stream_class);
        if(JS_NewClass(rt,stream_class,&stream_def)<0)return ESP_FAIL;
        stream_rt=rt;
    }
    JSValue proto=JS_NewObject(ctx);
    if(JS_IsException(proto))return ESP_ERR_NO_MEM;
    if(JS_SetPropertyFunctionList(ctx,proto,methods,2)<0) {
        JS_FreeValue(ctx,proto);return ESP_ERR_NO_MEM;
    }
    JS_SetClassProto(ctx,stream_class,proto);
    return JS_SetPropertyFunctionList(ctx,ns,functions,2)<0?ESP_ERR_NO_MEM:ESP_OK;
}
static const pocket_capability_t seed_cap={.name="random.seed",.supported=true,.available=true};
static const pocket_capability_t stream_cap={.name="random.stream",.supported=true,.available=true};
esp_err_t pocket_random_install(JSContext *ctx,void *user_data) {
    (void)user_data;
    // Sessions own one runtime at a time. Its address may be reused after
    // destruction, so pointer equality cannot carry class readiness across runs.
    stream_rt=NULL;
    stream_class=0;
    esp_err_t err=pocket_api_register(&seed_cap);
    if(err!=ESP_OK)return err;
    err=pocket_api_register(&stream_cap);
    if(err!=ESP_OK)return err;
    return pocket_api_lazy(ctx,"random",build_random,NULL);
}
