#include "pocket_clock.h"
#include "pocket_clock_source.h"
#include "pocket_kasane.h"
#include "pocket_api.h"
#include "system/sys_state.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// pocket_clock.c also serves the wall clock as a Kasane source; this test is
// about wall() alone, so the source path is linked against stubs that abort
// if it is ever reached. The signatures come from the real headers above.
static void unreached(const char *name){fprintf(stderr,"unexpected call: %s\n",name);abort();}
JSValue pocket_kasane_source_capability(JSContext *ctx,ksn_source_registry *registry,
                                         ksn_source_handle handle){
    (void)ctx;(void)registry;(void)handle;unreached(__func__);return JS_UNDEFINED;
}
void ksn_source_registry_init(ksn_source_registry *registry){(void)registry;unreached(__func__);}
ksn_result ksn_source_register(ksn_source_registry *registry,const ksn_source_provider *provider,
                               ksn_source_handle *out){
    (void)registry;(void)provider;(void)out;unreached(__func__);return KSN_INVALID;
}
ksn_result ksn_source_unregister(ksn_source_registry *registry,ksn_source_handle handle){
    (void)registry;(void)handle;unreached(__func__);return KSN_INVALID;
}
ksn_result pocket_clock_source_open(void *storage,ksn_source_provider *out){
    (void)storage;(void)out;unreached(__func__);return KSN_INVALID;
}
void pocket_clock_source_registered(void *storage,ksn_source_handle handle){
    (void)storage;(void)handle;unreached(__func__);
}
JSValue pocket_api_throw(JSContext *ctx,const char *code,const char *operation,
                         const char *message,bool retryable,const char *outcome){
    (void)ctx;(void)code;(void)operation;(void)message;(void)retryable;(void)outcome;
    unreached(__func__);return JS_EXCEPTION;
}
static sys_state clock_state;
static uint64_t mono;
bool sys_device_clock_read(sys_clock_state *out){return sys_clock_snapshot(&clock_state,mono,out);}
static void check(JSContext *ctx,const char *script){
    JSValue v=JS_Eval(ctx,script,strlen(script),"clock-test",JS_EVAL_TYPE_GLOBAL);
    if(JS_IsException(v)){
        JSValue e=JS_GetException(ctx);const char *s=JS_ToCString(ctx,e);
        fprintf(stderr,"%s\n",s?s:"exception");JS_FreeCString(ctx,s);JS_FreeValue(ctx,e);assert(0);
    }
    JS_FreeValue(ctx,v);
}
int main(void){
    for(int session=0;session<30;session++){
        JSRuntime *rt=JS_NewRuntime();JSContext *ctx=JS_NewContext(rt);assert(ctx);
        JSValue global=JS_GetGlobalObject(ctx);
        assert(JS_SetPropertyStr(ctx,global,"wall",JS_NewCFunction(ctx,pocket_clock_wall,"wall",0))>=0);
        JS_FreeValue(ctx,global);clock_state=(sys_state){0};mono=0;
        check(ctx,"function ck(v){if(!v)throw Error('clock contract');} ck(wall().unixMs===null&&wall().source==='unsynced');");
        assert(sys_clock_offer_pc(&clock_state,1800000000,0)==SYS_OK);mono=1234000;
        check(ctx,"ck(wall().unixMs===1800000001234&&wall().source==='host');");
        sys_clock_anchor a={.seconds=INT64_C(2524608000),.microseconds=500000,.mono_us=mono,.source=SYS_CLOCK_RTC};
        assert(sys_clock_update(&clock_state,a)==SYS_OK);
        check(ctx,"ck(wall().unixMs===2524608000500&&wall().source==='network');");
        a.source=SYS_CLOCK_SNTP;assert(sys_clock_update(&clock_state,a)==SYS_OK);
        check(ctx,"ck(wall().source==='network');");
        a.seconds=INT64_MAX;assert(sys_clock_update(&clock_state,a)==SYS_OK);
        check(ctx,"ck(wall().unixMs===null&&wall().source==='unsynced');");
        JS_FreeContext(ctx);JS_FreeRuntime(rt);
    }
    puts("POCKET_CLOCK_OK actual QuickJS, unsynced, PC, RTC/SNTP, 2050, safe-number bound, 30 sessions");
}
