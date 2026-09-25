#include "pocket_clock.h"
#include "pocket_clock_source.h"
#include "pocket_kasane.h"
#include "pocket_api.h"
#include "system/sys_device.h"
#include <math.h>
#include <stdlib.h>

typedef struct {
    ksn_source_registry registry;
    ksn_source_provider provider;
    pocket_clock_source_state source;
    ksn_source_handle handle;
} wall_source_service;
static wall_source_service *wall_source;
static bool wall_source_retained;

JSValue pocket_clock_wall(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    sys_clock_state sample;
    bool valid=sys_device_clock_read(&sample)&&sample.seconds<=INT64_C(9007199254740);
    JSValue object=JS_NewObject(ctx);
    if(JS_IsException(object))return object;
    if(JS_SetPropertyStr(ctx,object,"unixMs",valid?
        JS_NewFloat64(ctx,round(sample.seconds*1000.0+sample.microseconds/1000.0)):JS_NULL)<0||
       JS_SetPropertyStr(ctx,object,"source",JS_NewString(ctx,!valid?"unsynced":
           sample.source==SYS_CLOCK_PC?"host":"network"))<0){
        JS_FreeValue(ctx,object);return JS_EXCEPTION;
    }
    return object;
}

JSValue pocket_clock_wall_source(JSContext *ctx,JSValueConst this_val,
                                 int argc,JSValueConst *argv){
    (void)this_val;(void)argc;(void)argv;
    if(wall_source_retained){
        if(ksn_source_unregister(&wall_source->registry,wall_source->handle)!=KSN_OK)
            return pocket_api_throw(ctx,POCKET_ERR_BUSY,"time.wallSource",
                "a previous clock source still has readers",true,
                POCKET_OUTCOME_NOT_APPLIED);
        free(wall_source);
        wall_source=NULL;
        wall_source_retained=false;
    }
    if(!wall_source){
        wall_source_service *service=calloc(1,sizeof(*service));
        if(!service)return pocket_api_throw(ctx,POCKET_ERR_OUT_OF_MEMORY,
            "time.wallSource","clock source allocation failed",true,
            POCKET_OUTCOME_NOT_APPLIED);
        ksn_source_registry_init(&service->registry);
        ksn_result result=pocket_clock_source_open(&service->source,&service->provider);
        if(result==KSN_OK)result=ksn_source_register(&service->registry,
                                                       &service->provider,&service->handle);
        if(result!=KSN_OK){
            free(service);
            return pocket_api_throw(ctx,result==KSN_LIMIT?POCKET_ERR_LIMIT_EXCEEDED:
                POCKET_ERR_INVALID_ARGUMENT,"time.wallSource",
                "clock source registration failed",false,POCKET_OUTCOME_NOT_APPLIED);
        }
        pocket_clock_source_registered(&service->source,service->handle);
        wall_source=service;
    }
    return pocket_kasane_source_capability(ctx,&wall_source->registry,wall_source->handle);
}

void pocket_clock_reset(void){
    if(!wall_source)return;
    if(ksn_source_unregister(&wall_source->registry,wall_source->handle)!=KSN_OK){
        wall_source_retained=true;
        return;
    }
    free(wall_source);
    wall_source=NULL;
    wall_source_retained=false;
}
