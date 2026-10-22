/* Actual generic VM -> procedural adapter -> generated DERBY ROM -> frame.
 * Both builds use the same workload and engine. No prepared-call machinery. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <float.h>
#include <math.h>
#include <stdbool.h>
#ifdef NUMBER_ACCOUNTING
#include <malloc.h>
#include "quickjs.h"
static bool accounting;
static uint64_t js_calls,js_bytes,native_calls,native_bytes;
static void record(uint64_t *calls,uint64_t *bytes,size_t n){if(accounting){(*calls)++;*bytes+=n;}}
static void *native_malloc(size_t n){record(&native_calls,&native_bytes,n);return malloc(n);}
static void *native_calloc(size_t n,size_t z){record(&native_calls,&native_bytes,n*z);return calloc(n,z);}
static void *js_alloc(void *o,size_t n){(void)o;record(&js_calls,&js_bytes,n);return malloc(n);}
static void *js_zero(void *o,size_t n,size_t z){(void)o;record(&js_calls,&js_bytes,n*z);return calloc(n,z);}
static void *js_resize(void *o,void *p,size_t n){(void)o;if(n)record(&js_calls,&js_bytes,n);return realloc(p,n);}
static void js_release(void *o,void *p){(void)o;free(p);}
static size_t js_usable(const void *p){return p?malloc_usable_size((void *)p):0;}
static const JSMallocFunctions allocators={js_zero,js_alloc,js_release,js_resize,js_usable};
#define malloc native_malloc
#define calloc native_calloc
#endif
#include PROC_SOURCE
#ifdef NUMBER_ACCOUNTING
#undef malloc
#undef calloc
#define ACCOUNT(x) (accounting=(x))
#else
#define ACCOUNT(x) ((void)0)
#endif
#define CHECK(x) do{if(!(x)){fprintf(stderr,"LENGTH_BENCH_FAIL %d: %s\n",__LINE__,#x);abort();}}while(0)
ksn_result pocket_kasane_proc_publish(void){return KSN_OK;}
ksn_result pocket_kasane_proc_publish_at(unsigned s,ksn_rect r){(void)s;(void)r;return KSN_OK;}
void pocket_kasane_invalidate(void){}
JSValue pocket_kasane_proc_resource(JSContext *ctx){(void)ctx;return JS_UNDEFINED;}
JSValue pocket_kasane_proc_resource_at(JSContext *ctx,unsigned s){(void)ctx;(void)s;return JS_UNDEFINED;}
JSValue pocket_api_throw(JSContext *ctx,const char *c,const char *op,const char *msg,bool r,const char *o){(void)r;(void)o;return JS_ThrowTypeError(ctx,"%s %s %s",c,op,msg);}
static uint64_t clock_ns(clockid_t clock){struct timespec t;clock_gettime(clock,&t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
static uint64_t hash;
static unsigned frames;
static JSValue ack(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)ctx;(void)self;(void)argc;(void)argv;
    CHECK(surfaces[0].pending);pocket_proc_present_result(KSN_OK);
    const ksn_proc_frame *f=surfaces[0].committed;CHECK(f&&f->count==1);
    const ksn_proc_segment *s=&f->segments[0];
    const int fields[]={s->x0,s->y0,s->x1,s->y1,s->color,f->raster_steps};
    for(unsigned i=0;i<sizeof fields/sizeof fields[0];i++)hash=(hash^(uint32_t)fields[i])*UINT64_C(1099511628211);
    frames++;return JS_UNDEFINED;
}
static JSValue eval(JSContext *ctx,const char *source){
    JSValue v=JS_Eval(ctx,source,strlen(source),"length-read.js",JS_EVAL_TYPE_GLOBAL);
    if(JS_IsException(v)){JSValue e=JS_GetException(ctx);const char *s=JS_ToCString(ctx,e);fprintf(stderr,"%s\n",s?s:"exception");abort();}
    return v;
}
static JSContext *setup(JSRuntime *rt){
    JSContext *ctx=JS_NewContext(rt);CHECK(ctx);
    JSValue p=JS_NewObject(ctx),k=JS_NewObject(ctx),g=JS_GetGlobalObject(ctx);
    CHECK(pocket_proc_install(ctx,k)==ESP_OK);
    CHECK(JS_SetPropertyStr(ctx,p,"kasane",k)>=0);CHECK(JS_SetPropertyStr(ctx,g,"pocket",p)>=0);
    CHECK(JS_SetPropertyStr(ctx,g,"ack",JS_NewCFunction(ctx,ack,"ack",0))>=0);JS_FreeValue(ctx,g);
    pocket_proc_rom_plans(ksn_proc_rom_plans,ksn_proc_rom_plans_count);next_handle=0;return ctx;
}
/* One differential boundary matrix, including a coercion trap that must never run. */
static void boundaries(JSContext *ctx){
    static const char src[]="globalThis.coerced=0;[0,-0,1,-1,2147483647,-2147483648,1.25,Number.MIN_VALUE,Number.MAX_VALUE,NaN,Infinity,-Infinity,undefined,null,true,'1',1n,Symbol('x'),{valueOf(){coerced++;throw Error('coercion')}}]";
    JSValue a=eval(ctx,src);
    for(unsigned i=0;i<19;i++){
        JSValue v=JS_GetPropertyUint32(ctx,a,i);double before=123,after=123;
        bool expected=JS_IsNumber(v)&&JS_ToFloat64(ctx,&before,v)==0&&isfinite(before);
        bool got=number(ctx,v,&after);CHECK(got==expected);
        if(got)CHECK(!memcmp(&before,&after,sizeof before));
        JS_FreeValue(ctx,v);
    }
    JS_FreeValue(ctx,a);JSValue check=eval(ctx,"coerced===0");CHECK(JS_ToBool(ctx,check)==1);JS_FreeValue(ctx,check);

    static const char *arrays[]={"[]","[1]","new Array(2147483647)","new Array(2147483648)","new Array(4294967295)","Object.freeze([1,2])"};
    for(unsigned i=0;i<sizeof arrays/sizeof arrays[0];i++){
        JSValue a=eval(ctx,arrays[i]),v=JS_GetPropertyStr(ctx,a,"length");uint32_t expected=0,got=0;
        CHECK(integer(ctx,v,UINT32_MAX,&expected));JS_FreeValue(ctx,v);
        CHECK(array_length(ctx,a,&got)&&got==expected);JS_FreeValue(ctx,a);
    }
    static const char *rejected[]={"new Proxy([],{get(){coerced++;throw Error('trap')}})","new Uint8Array(1)","({get length(){coerced++;throw Error('trap')}})"};
    for(unsigned i=0;i<sizeof rejected/sizeof rejected[0];i++){
        JSValue a=eval(ctx,rejected[i]);uint32_t ignored=0;CHECK(!array_length(ctx,a,&ignored));JS_FreeValue(ctx,a);
    }
    JSValue check_length=eval(ctx,"coerced===0");CHECK(JS_ToBool(ctx,check_length)==1);JS_FreeValue(ctx,check_length);

}
int main(int argc,char **argv){
    CHECK(argc==3);const char *kind=argv[1];unsigned n=(unsigned)strtoul(argv[2],NULL,10);CHECK(n&&n<=100000);
    #ifdef NUMBER_ACCOUNTING
    JSRuntime *rt=JS_NewRuntime2(&allocators,NULL);
#else
    JSRuntime *rt=JS_NewRuntime();
#endif
    CHECK(rt);JSContext *ctx=setup(rt);boundaries(ctx);
    uint64_t ns=0,cpu_ns=0;hash=UINT64_C(1469598103934665603);frames=0;
    if(!strcmp(kind,"startup")){
        JSValue v;uint64_t cpu_start=clock_ns(CLOCK_THREAD_CPUTIME_ID),start=clock_ns(CLOCK_MONOTONIC);ACCOUNT(true);
        v=eval(ctx,"pocket.kasane.procedural.register('derby.pt',[512]);");ACCOUNT(false);ns=clock_ns(CLOCK_MONOTONIC)-start;cpu_ns=clock_ns(CLOCK_THREAD_CPUTIME_ID)-cpu_start;
        int32_t h;CHECK(!JS_ToInt32(ctx,&h,v)&&h==1);JS_FreeValue(ctx,v);
        CHECK(slots[0].rom&&((ksn_proc_rom_plan *)slots[0].plan)->args[0]==512);hash=512;n=1;
    }else{
        bool array=!strcmp(kind,"array");CHECK(array||!strcmp(kind,"numeric"));
        char source[768];
        snprintf(source,sizeof source,"const p=pocket.kasane.procedural;const h=p.register('derby.pt',[512]);function batch(n){for(let i=0;i<n;i++){p.beginFrame(0);p.draw(h,%s);p.commit();ack();}return n;}",array?"[40.25+(i&1)*0.5,10,-10,0,1,0.1,100,0]":"40.25+(i&1)*0.5,10,-10,0,1,0.1,100,0");
        JS_FreeValue(ctx,eval(ctx,source));
        JSValue g=JS_GetGlobalObject(ctx),fn=JS_GetPropertyStr(ctx,g,"batch");JS_FreeValue(ctx,g);
        JSValue count=JS_NewInt32(ctx,(int32_t)n);uint64_t cpu_start=clock_ns(CLOCK_THREAD_CPUTIME_ID),start=clock_ns(CLOCK_MONOTONIC);ACCOUNT(true);
        JSValue result=JS_Call(ctx,fn,JS_UNDEFINED,1,&count);ACCOUNT(false);ns=clock_ns(CLOCK_MONOTONIC)-start;cpu_ns=clock_ns(CLOCK_THREAD_CPUTIME_ID)-cpu_start;
        CHECK(!JS_IsException(result));int32_t done;CHECK(!JS_ToInt32(ctx,&done,result)&&done==(int32_t)n&&frames==n);
        JS_FreeValue(ctx,result);JS_FreeValue(ctx,fn);
    }
    printf("{\"case\":\"%s\",\"n\":%u,\"ns\":%llu,\"cpu_ns\":%llu,\"hash\":\"%016llx\",\"pointer_bytes\":%zu,\"boundaries\":28}\n",kind,n,(unsigned long long)ns,(unsigned long long)cpu_ns,(unsigned long long)hash,sizeof(void *));
#ifdef NUMBER_ACCOUNTING
    fprintf(stderr,"{\"js_calls\":%llu,\"js_requested_bytes\":%llu,\"adapter_calls\":%llu,\"adapter_requested_bytes\":%llu}\n",(unsigned long long)js_calls,(unsigned long long)js_bytes,(unsigned long long)native_calls,(unsigned long long)native_bytes);
#endif
    pocket_proc_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);return 0;
}
