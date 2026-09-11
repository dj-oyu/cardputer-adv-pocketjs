#include "pocket_random.h"
#include "pocket_api.h"
#include "esp_app_desc.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int64_t esp_timer_get_time(void) {return 1000000;}
static const esp_app_desc_t description={.version="host"};
const esp_app_desc_t *esp_app_get_description(void) {return &description;}
static void eval(JSContext *ctx,const char *script) {
    JSValue v=JS_Eval(ctx,script,strlen(script),"random-test",JS_EVAL_TYPE_GLOBAL);
    if(JS_IsException(v)) {
        JSValue e=JS_GetException(ctx);const char *s=JS_ToCString(ctx,e);
        fprintf(stderr,"%s\n",s?s:"exception");
        JS_FreeCString(ctx,s);JS_FreeValue(ctx,e);assert(0);
    }
    JS_FreeValue(ctx,v);
}
int main(void) {
    for(int session=0;session<30;session++) {
        JSRuntime *rt=JS_NewRuntime();assert(rt);
        JSContext *ctx=JS_NewContext(rt);assert(ctx);
        // Different registration order and reused runtime addresses must work.
        for(int i=0;i<session%5;i++) {JSClassID id=0;JS_NewClassID(rt,&id);}
        assert(pocket_api_install(ctx,NULL)==ESP_OK);
        assert(pocket_random_install(ctx,NULL)==ESP_OK);
        eval(ctx,
            "function check(v){if(!v)throw Error('random contract');}"
            "check(pocket.capabilities.get('random.seed').available);"
            "check(pocket.capabilities.get('random.stream').supported);"
            "let legacy=Math.random; let r=pocket.random; check(Math.random===legacy);"
            "let seed=r.seed();check(Number.isInteger(seed)&&seed>=0&&seed<=4294967295);"
            "let a=r.create(1),b=r.create(1);"
            "for(let n of [270369,67634689,2647435461,307599695,2398689233])check(a.nextUint32()===n);"
            "check(b.nextUint32()===270369);"
            "let z=r.create(0),q=r.create(0x6D2B79F5);"
            "for(let i=0;i<100;i++)check(z.nextUint32()===q.nextUint32());"
            "a=r.create(4294967295);b=r.create(4294967295);"
            "for(let i=0;i<10000;i++){let f=a.nextFloat();check(f>=0&&f<1&&f===(b.nextUint32()>>>8)/16777216);}"
            "for(let bad of [undefined,null,'1',true,{},1n,NaN,Infinity,-1,0.5,4294967296]){"
            "let rejected=false;try{r.create(bad);}catch(e){rejected=e.code==='INVALID_ARGUMENT';}check(rejected);}"
            "let rejected=false;try{a.nextUint32.call({});}catch(e){rejected=true;}check(rejected);"
            "for(let i=0;i<1000;i++)r.create(i).nextUint32();");
        JS_RunGC(rt);
        pocket_api_reset();
        JS_FreeContext(ctx);JS_FreeRuntime(rt);
    }
    puts("POCKET_RANDOM_OK: JS/native vectors, validation, capabilities, 30 sessions and GC");
}
