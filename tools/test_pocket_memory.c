#include "pocket_memory.h"
#include "quickjs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static JSContext *ctx;

static void check(const char *source) {
    JSValue value=JS_Eval(ctx,source,strlen(source),"memory-test.js",JS_EVAL_TYPE_GLOBAL);
    if(JS_IsException(value)) {
        JSValue error=JS_GetException(ctx);
        const char *message=JS_ToCString(ctx,error);
        fprintf(stderr,"exception: %s in %s\n",message?message:"?",source);
        if(message)JS_FreeCString(ctx,message);
        JS_FreeValue(ctx,error);
        assert(0);
    }
    int valid=JS_ToBool(ctx,value);
    JS_FreeValue(ctx,value);
    assert(valid==1);
}

int main(void) {
    JSRuntime *rt=JS_NewRuntime();
    assert(rt);
    ctx=JS_NewContext(rt);
    assert(ctx);
    assert(pocket_memory_install(ctx,NULL)==ESP_OK);
    check("globalThis.events=[];globalThis.sub=memory.onPressure((m,e)=>events.push([m,e]));"
          "memory.pressure()===0 && events.length===0");
    pocket_memory_sample(1000000,80*1024,100*1024,false,0,0);
    assert(pocket_memory_native_sample_due(1000000));
    pocket_memory_pump(false);
    check("memory.pressure()===1 && events.length===1 && events[0][0]===1 && events[0][1]===1");
    pocket_memory_sample(1100000,20*1024,100*1024,false,0,0);
    pocket_memory_pump(true);
    check("events.length===1 && memory.pressure()===1");
    pocket_memory_sample(1600000,20*1024,100*1024,false,0,0);
    pocket_memory_pump(false);
    check("events.length===2 && events[1][0]===0 && events[1][1]===1");
    pocket_memory_sample(1700000,20*1024,100*1024,true,20*1024,10*1024);
    pocket_memory_pump(false);
    check("events.length===3 && events[2][0]===6 && events[2][1]===2");
    JSOOMCanary canary={.count=1,.quota_count=1,.first_req=999,.first_used=123};
    pocket_memory_oom(&canary,1800000);
    pocket_memory_pump(false);
    check("events.length===4 && events[3][0]===14 && memory.info().quotaFailures===1");
    check("sub.close();sub.close();globalThis.other=memory.onPressure((m,e)=>events.push([m,e]));"
          "events.length===4");
    pocket_memory_pump(false);
    check("events.length===5 && events[4][0]===14 && events[4][1]===2");
    check("other.close();globalThis.bad=memory.onPressure(()=>{throw Error('bad')});true");
    pocket_memory_sample(1900000,20*1024,100*1024,true,20*1024,10*1024);
    pocket_memory_pump(false);
    check("memory.info().callbackFailures===1 && memory.pressure()===14");
    check("globalThis.after=memory.onPressure(()=>{});after.close();true");
    pocket_memory_native_failure(4096,0);
    pocket_memory_sample(2000000,20*1024,100*1024,false,0,0);
    check("memory.info().nativeFailures===1 && (memory.info().failureReasons&4)===4");
    size_t used=0;
    JS_GetMemoryCounters(rt,&used,NULL);
    JS_SetMemoryLimit(rt,used+1);
    JSValue refused=JS_NewStringLen(ctx,"this must exceed the quota",26);
    JSOOMCanary actual={0};
    JS_TakeOOMCanary(rt,&actual);
    JS_SetMemoryLimit(rt,0);
    assert(JS_IsException(refused) && actual.count && actual.quota_count &&
           !actual.allocator_count);
    JS_FreeValue(ctx,refused);
    if(JS_HasException(ctx))JS_FreeValue(ctx,JS_GetException(ctx));
    pocket_memory_reset();
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    puts("POCKET_MEMORY_HOST PASS");
    return 0;
}
