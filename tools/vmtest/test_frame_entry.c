#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "pocketjs/guest_quickjs.h"
#include "quickjs-vm.h"
#include "jsconsole.h"
extern void vmtest_vm_set_force_yield(JSRuntime *, int);
int64_t esp_timer_get_time(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (int64_t)t.tv_sec*1000000+t.tv_nsec/1000;}
static unsigned reports;
static void report(JSContext *ctx,JSValueConst exception,void *opaque){(void)opaque;assert(!JS_VMSuspended(JS_GetRuntime(ctx)));reports++;jsconsole_report_exception(ctx,exception);}
static void run(const char *source,int expect_error,int count){
 pocketjs_guest_config_t cfg;pocketjs_guest_config_defaults(&cfg);pocketjs_guest_t *g=NULL;assert(pocketjs_guest_create(&cfg,&g)==ESP_OK);
 pocketjs_guest_yield_enabled(g,false);pocketjs_guest_set_frame_error_handler(g,report,NULL);
 assert(pocketjs_guest_quickjs_install_once(g,"console",jsconsole_install,NULL)==ESP_OK);
 jsconsole_clear();reports=0;assert(pocketjs_guest_eval(g,source,strlen(source),"entry.js")==ESP_OK);
 JSContext *ctx=pocketjs_guest_quickjs_context(g);JSRuntime *rt=JS_GetRuntime(ctx);vmtest_vm_set_force_yield(rt,count!=0);
 pocketjs_guest_frame_t input={.struct_size=sizeof(input)};esp_err_t e=pocketjs_guest_frame(g,&input);unsigned resumes=0;
 while(e==ESP_OK&&pocketjs_guest_work_pending(g)){assert(reports==0);assert(++resumes<10000);JS_RunGC(rt);e=pocketjs_guest_continue(g);}
 assert((e!=ESP_OK)==expect_error);assert(reports==(unsigned)expect_error);
 JSValue glob=JS_GetGlobalObject(ctx),value=JS_GetPropertyStr(ctx,glob,"sum");int32_t sum;assert(JS_ToInt32(ctx,&sum,value)==0);assert(sum==count*(count-1)/2);JS_FreeValue(ctx,value);JS_FreeValue(ctx,glob);
 if(expect_error)assert(jsconsole_error()&&strstr(jsconsole_error(),"boom")&&strstr(jsconsole_error(),"entry.js"));
 assert(resumes>=(unsigned)(count*2));printf("FRAME_ENTRY PASS loops=%d resumes=%u reports=%u error=%s\n",count,resumes,reports,jsconsole_error()?jsconsole_error():"none");
 vmtest_vm_set_force_yield(rt,0);pocketjs_guest_destroy(g);
}
int main(void){
 run("globalThis.sum=0;globalThis.frame=function(){for(let i=0;i<100;i++)sum+=i;}",0,100);
 run("globalThis.sum=0;globalThis.frame=function(){for(let i=0;i<1000;i++)sum+=i;}",0,1000);
 run("globalThis.sum=0;globalThis.frame=function(){'use strict';if(this!==globalThis)throw Error('receiver');for(let i=0;i<100;i++)sum+=i;if(this!==globalThis)throw Error('resumed receiver');}",0,100);
 run("globalThis.sum=0;globalThis.frame=function(){for(let i=0;i<100;i++)sum+=i;throw new Error('boom');}",1,100);
 run("globalThis.sum=0;globalThis.frame=function(){throw new Error('boom');}",1,0);
 /* A stop closes a parked frame without exposing it as a frame failure. */
 pocketjs_guest_config_t cfg;pocketjs_guest_config_defaults(&cfg);pocketjs_guest_t *g=NULL;assert(pocketjs_guest_create(&cfg,&g)==ESP_OK);
 pocketjs_guest_yield_enabled(g,false);pocketjs_guest_set_frame_error_handler(g,report,NULL);reports=0;
 const char *s="globalThis.frame=function(){for(;;){}}";assert(pocketjs_guest_eval(g,s,strlen(s),"stop.js")==ESP_OK);
 JSRuntime *rt=JS_GetRuntime(pocketjs_guest_quickjs_context(g));vmtest_vm_set_force_yield(rt,1);
 pocketjs_guest_frame_t input={.struct_size=sizeof(input)};assert(pocketjs_guest_frame(g,&input)==ESP_OK);assert(pocketjs_guest_suspended(g));
 pocketjs_guest_prepare_stop(g);assert(!pocketjs_guest_suspended(g));assert(reports==0);vmtest_vm_set_force_yield(rt,0);pocketjs_guest_destroy(g);
 puts("FRAME_ENTRY STOP PASS");
 /* Formatting an arbitrary thrown value must not leave a new exception. */
 rt=JS_NewRuntime();JSContext *ctx=JS_NewContext(rt);
 const char *bad="throw {toString(){throw Error('string getter');},get stack(){throw Error('stack getter');}}";
 JSValue v=JS_Eval(ctx,bad,strlen(bad),"format.js",JS_EVAL_TYPE_GLOBAL);assert(JS_IsException(v));
 JSValue exception=JS_GetException(ctx);jsconsole_clear();jsconsole_report_exception(ctx,exception);
 assert(!JS_HasException(ctx));assert(strcmp(jsconsole_error(),"frame failed")==0);
 JS_FreeValue(ctx,exception);JS_FreeValue(ctx,v);JS_FreeContext(ctx);JS_FreeRuntime(rt);puts("FRAME_ENTRY FORMAT PASS");
 puts("FRAME_ENTRY ALL PASS");return 0;
}
