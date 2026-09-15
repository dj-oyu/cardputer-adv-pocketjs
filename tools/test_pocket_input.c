#include "pocket_input.h"
#include "pocket_api.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
/* Generated verbatim from pocket_api.c: use production subscription semantics. */
#include "pocket_sub_impl.inc"
void host_capabilities_clear(void);
static int64_t clock_us;
int64_t esp_timer_get_time(void){return clock_us;}
static JSContext *ctx;
static unsigned failures;
static void check(const char *code){
    JSValue value=JS_Eval(ctx,code,strlen(code),"input-test",JS_EVAL_TYPE_GLOBAL);
    if(JS_IsException(value)){
        JSValue e=JS_GetException(ctx);const char *s=JS_ToCString(ctx,e);
        fprintf(stderr,"FAIL %s: %s\n",code,s?s:"?");failures++;
        if(s)JS_FreeCString(ctx,s);
        JS_FreeValue(ctx,e);
    }
    JS_FreeValue(ctx,value);
}
static void pump(int64_t us,uint32_t mask){clock_us=us;pocket_input_pump(mask);}
int main(void){
    for(unsigned round=0;round<2;round++){
        JSRuntime *rt=JS_NewRuntime();ctx=JS_NewContext(rt);host_capabilities_clear();
        if(pocket_input_install(ctx,NULL)!=ESP_OK)return 1;
        check("if(typeof ui!=='undefined')throw Error('node dependency');"
              "var events=[],bad=0;var s=input.onAction(e=>events.push(e));"
              "var throwing=input.onAction(()=>{bad++;throw Error('listener')});");
        pump(1000,0x4000);pump(400999,0x4000);
        check("if(events.length!==1||events[0].phase!=='press'||events[0].action!=='accept'||"
              "events[0].timeMs!==1||bad!==1||!input.held('accept'))throw Error('press/delay');");
        pump(401000,0x4000);pump(520999,0x4000);pump(521000,0x4000);pump(522000,0);
        check("if(events.map(e=>e.phase).join()!=='press,repeat,repeat,release'||bad!==1||"
              "input.held('accept'))throw Error('repeat/release/throw');s.close();s.close();");
        pump(600000,0x10);
        check("if(!input.held('up'))throw Error('held without listener');"
              "var self=0;var once=input.onAction(()=>{self++;once.close()});");
        pump(610000,0);pump(620000,0x4000);
        check("if(self!==1)throw Error('self close');"
              "var subs=[];for(var i=0;i<4;i++)subs.push(input.onAction(()=>{}));"
              "var full=false;try{input.onAction(()=>{})}catch(e){full=e.code==='LIMIT_EXCEEDED'}"
              "if(!full)throw Error('quota');subs.forEach(s=>s.close());");
        pocket_input_reset();
        check("if(input.held('accept')||input.held('up'))throw Error('reset held');");
        if(pocket_input_install(ctx,NULL)!=ESP_OK)return 1;
        check("var after=0;var current=input.onAction(()=>after++);s.close();once.close();");
        pump(1000000,0x4000);pump(1100000,0x4000);
        check("if(after!==1)throw Error('stale close or stale repeat');"
              "var unsupported=false;try{input.onKey(()=>{})}catch(e){unsupported=e.code==='UNSUPPORTED'}"
              "if(!unsupported)throw Error('onKey');");
        pocket_input_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);
    }
    printf("input service: %s (no nodes, held, repeat boundaries, throwing/self-closing listeners, quota, reset)\n",failures?"FAIL":"PASS");
    return failures?1:0;
}
