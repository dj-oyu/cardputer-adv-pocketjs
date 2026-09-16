#include "pocket_clock.h"
#include "system/sys_device.h"
#include <math.h>

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
