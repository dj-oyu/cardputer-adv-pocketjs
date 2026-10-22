/* Actual adapter + actual engine dispatcher. Descriptor and ROM inputs are
 * generated from source, never hand-built trusted constant specializations. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include "quickjs-prepared.h"
static unsigned allocations;
static bool fail_native;
static void *test_malloc(size_t n){allocations++;return fail_native?NULL:malloc(n);}
static void *test_calloc(size_t n,size_t s){allocations++;return fail_native?NULL:calloc(n,s);}
#define malloc test_malloc
#define calloc test_calloc
#include "pocket_proc.c"
#undef malloc
#undef calloc
#define CHECK(x) do{if(!(x)){fprintf(stderr,"runtime.c:%d: %s\n",__LINE__,#x);abort();}}while(0)
ksn_result pocket_kasane_proc_publish(void){return KSN_OK;}
ksn_result pocket_kasane_proc_publish_at(unsigned s,ksn_rect r){(void)s;(void)r;return KSN_OK;}
void pocket_kasane_invalidate(void){}
JSValue pocket_kasane_proc_resource(JSContext *ctx){(void)ctx;return JS_UNDEFINED;}
JSValue pocket_kasane_proc_resource_at(JSContext *ctx,unsigned n){(void)ctx;(void)n;return JS_UNDEFINED;}
JSValue pocket_api_throw(JSContext *ctx,const char *code,const char *op,const char *msg,bool r,const char *out){
    (void)r;(void)out;return JS_ThrowTypeError(ctx,"%s %s %s",code,op,msg);
}
static JSValue evaluate(JSContext *ctx,const char *text,bool prepared){
    return prepared?pocket_proc_eval_prepared(ctx,text,strlen(text),"runtime-startup.js"):
                    JS_Eval(ctx,text,strlen(text),"runtime-startup.js",JS_EVAL_TYPE_GLOBAL);
}
static int integer_eval(JSContext *ctx,const char *text){
    JSValue v=evaluate(ctx,text,false);int32_t n;
    CHECK(!JS_IsException(v));CHECK(!JS_ToInt32(ctx,&n,v));JS_FreeValue(ctx,v);return n;
}
static void ok(JSContext *ctx,const char *text,bool prepared){
    JSValue v=evaluate(ctx,text,prepared);
    if(JS_IsException(v)){
        JSValue e=JS_GetException(ctx);const char *s=JS_ToCString(ctx,e);
        fprintf(stderr,"%s\n",s?s:"exception");abort();
    }
    JS_FreeValue(ctx,v);CHECK(!prepared_active&&!prepared_consuming);
}
static JSValue collect(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argc;(void)argv;JS_RunGC(JS_GetRuntime(ctx));return JS_UNDEFINED;
}
static JSValue rooted(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argc;(void)argv;
    CHECK(prepared_active&&prepared_active->has_binding);
    return JS_DupValue(ctx,prepared_active->binding);
}
static ksn_proc_rom_entry replacement;
static JSValue replace_registry(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)ctx;(void)self;(void)argc;(void)argv;
    pocket_proc_rom_plans(&replacement,1);return JS_UNDEFINED;
}
static JSContext *setup(JSRuntime *rt){
    JSContext *ctx=JS_NewContext(rt);CHECK(ctx);
    JSValue p=JS_NewObject(ctx),k=JS_NewObject(ctx),g=JS_GetGlobalObject(ctx);
    CHECK(pocket_proc_install(ctx,k)==ESP_OK);
    CHECK(JS_SetPropertyStr(ctx,p,"kasane",k)>=0);
    CHECK(JS_SetPropertyStr(ctx,g,"pocket",p)>=0);
    JS_SetPropertyStr(ctx,g,"gc",JS_NewCFunction(ctx,collect,"gc",0));
    JS_SetPropertyStr(ctx,g,"rooted",JS_NewCFunction(ctx,rooted,"rooted",0));
    JS_SetPropertyStr(ctx,g,"replaceRegistry",JS_NewCFunction(ctx,replace_registry,"replaceRegistry",0));
    JS_FreeValue(ctx,g);pocket_proc_rom_plans(ksn_proc_rom_plans,ksn_proc_rom_plans_count);
    allocations=prepared_hits=prepared_misses=0;return ctx;
}
static const char source[]="pocket.kasane.procedural.register('derby.pt',[512]);";
static JSValue captured(JSContext *ctx){
    JSValue g=JS_GetGlobalObject(ctx),p=JS_GetPropertyStr(ctx,g,"pocket");
    JSValue k=JS_GetPropertyStr(ctx,p,"kasane"),h=JS_GetPropertyStr(ctx,k,"procedural");
    JSValue f=JS_GetPropertyStr(ctx,h,"register");
    JS_FreeValue(ctx,h);JS_FreeValue(ctx,k);JS_FreeValue(ctx,p);JS_FreeValue(ctx,g);return f;
}
static unsigned reads;
static JSValue arg_getter(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argc;(void)argv;reads++;return JS_NewInt32(ctx,800);
}
static void getter_and_oom(JSContext *ctx){
    JSValue compiled=JS_Eval(ctx,source,strlen(source),"runtime-startup.js",
                            JS_EVAL_TYPE_GLOBAL|JS_EVAL_FLAG_COMPILE_ONLY);
    PJSPreparedCall c;CHECK(pjs_prepare_literal_call(ctx,compiled,&c));
    const ksn_proc_rom_entry *rom=NULL;
    for(unsigned i=0;i<rom_count;i++)if(!strcmp(rom_table[i].name,c.name))rom=&rom_table[i];
    CHECK(rom);
    prepared_scope scope={.ctx=ctx,.call=&c,.table=rom_table,.rom=rom,.count=rom_count};
    JSValue fn=captured(ctx),a=JS_NewArray(ctx),name=JS_NewString(ctx,c.name),out;

    JSAtom zero=JS_NewAtom(ctx,"0");
    JS_DefinePropertyGetSet(ctx,a,zero,JS_NewCFunction(ctx,arg_getter,"get",0),
                            JS_UNDEFINED,JS_PROP_CONFIGURABLE|JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx,zero);
    /* Defining an indexed property updates length exactly as normal JS. */
    JSValue args[]={name,a};prepared_active=&scope;reads=0;fail_native=true;
    scope.binding=JS_DupValue(ctx,fn);scope.has_binding=true;
    CHECK(pjs_prepared_dispatch(ctx,fn,JS_UNDEFINED,2,args,c.bytecode,c.next_pc,&out));
    CHECK(JS_IsException(out)&&reads==0);JS_FreeValue(ctx,JS_GetException(ctx));
    fail_native=false;
    scope.binding=JS_DupValue(ctx,fn);scope.has_binding=true;
    CHECK(pjs_prepared_dispatch(ctx,fn,JS_UNDEFINED,2,args,c.bytecode,c.next_pc,&out));
    CHECK(!JS_IsException(out)&&reads==1&&prepared_misses==1);JS_FreeValue(ctx,out);
    /* Failed site/arity guards don't inspect original arguments at all. */
    if(scope.has_binding)JS_FreeValue(ctx,scope.binding);
    scope.binding=JS_DupValue(ctx,fn);scope.has_binding=true;
    CHECK(!pjs_prepared_dispatch(ctx,fn,JS_UNDEFINED,1,args,c.bytecode,c.next_pc,&out));
    if(scope.has_binding)JS_FreeValue(ctx,scope.binding);
    scope.binding=JS_DupValue(ctx,fn);scope.has_binding=true;
    CHECK(!pjs_prepared_dispatch(ctx,fn,JS_UNDEFINED,2,args,c.bytecode,c.next_pc+1,&out));
    CHECK(reads==1);
    JSValue fake=JS_NewCFunction2(ctx,js_register,"register",1,JS_CFUNC_generic,
                                 PJS_PREPARED_REGISTER_MAGIC);
    if(scope.has_binding)JS_FreeValue(ctx,scope.binding);
    scope.binding=JS_DupValue(ctx,fn);scope.has_binding=true;
    CHECK(!pjs_prepared_dispatch(ctx,fake,JS_UNDEFINED,2,args,c.bytecode,c.next_pc,&out));
    JS_FreeValue(ctx,fake);
    JSContext *foreign=JS_NewContext(JS_GetRuntime(ctx));CHECK(foreign);
    if(scope.has_binding)JS_FreeValue(ctx,scope.binding);
    scope.binding=JS_DupValue(ctx,fn);scope.has_binding=true;
    CHECK(!pjs_prepared_dispatch(foreign,fn,JS_UNDEFINED,2,args,c.bytecode,c.next_pc,&out));
    JS_FreeContext(foreign);
    const ksn_proc_rom_entry *saved=rom_table;unsigned saved_count=rom_count;
    pocket_proc_rom_plans(NULL,0);
    if(scope.has_binding)JS_FreeValue(ctx,scope.binding);
    scope.binding=JS_DupValue(ctx,fn);scope.has_binding=true;
    CHECK(!pjs_prepared_dispatch(ctx,fn,JS_UNDEFINED,2,args,c.bytecode,c.next_pc,&out));
    pocket_proc_rom_plans(saved,saved_count);
    CHECK(reads==1);prepared_active=NULL;
    if(scope.has_binding)JS_FreeValue(ctx,scope.binding);JS_FreeValue(ctx,name);JS_FreeValue(ctx,a);JS_FreeValue(ctx,fn);JS_FreeValue(ctx,compiled);
}
static void root_lifetime_test(void){
    JSRuntime *rt=JS_NewRuntime();CHECK(rt);JSContext *ctx=setup(rt);
    JSValue compiled=JS_Eval(ctx,source,strlen(source),"scope.js",
                            JS_EVAL_TYPE_GLOBAL|JS_EVAL_FLAG_COMPILE_ONLY);
    PJSPreparedCall c;CHECK(pjs_prepare_literal_call(ctx,compiled,&c));
    prepared_scope scope={.ctx=ctx,.call=&c,.table=rom_table,.count=rom_count};
    for(unsigned i=0;i<rom_count;i++)if(!strcmp(rom_table[i].name,c.name))scope.rom=&rom_table[i];
    prepared_active=&scope;
    JSValue fn=captured(ctx);CHECK(!scope.has_binding);
    pjs_prepared_binding_created(ctx,fn,c.bytecode,c.lookup_pc+1);CHECK(!scope.has_binding);
    pjs_prepared_binding_created(ctx,fn,c.bytecode,c.lookup_pc);CHECK(scope.has_binding);
    JS_FreeValue(ctx,fn);
    JSValue ignored=evaluate(ctx,"pocket.kasane.procedural.register=function(){return 7;};gc();",false);
    CHECK(!JS_IsException(ignored));JS_FreeValue(ctx,ignored);
    CHECK(JS_IsLiveObject(rt,scope.binding));
    JSValue args[]={JS_NewString(ctx,c.name),JS_NewArray(ctx)},out;
    JS_SetPropertyUint32(ctx,args[1],0,JS_NewInt32(ctx,512));
    CHECK(pjs_prepared_dispatch(ctx,scope.binding,JS_UNDEFINED,2,args,c.bytecode,c.next_pc,&out));
    CHECK(!scope.has_binding&&!JS_IsException(out)&&prepared_hits==1);
    JS_FreeValue(ctx,out);JS_FreeValue(ctx,args[0]);JS_FreeValue(ctx,args[1]);
    prepared_active=NULL;JS_FreeValue(ctx,compiled);
    pocket_proc_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);
}
/* Fail only the original argument-array allocation phase. The lazy callee
 * is materialized first inside the same source, so this deliberately takes
 * generic fallback in both modes. Preparation must add no engine allocation. */
static unsigned engine_requests,fail_request;
static bool engine_armed;
static bool fail_engine(void){return engine_armed&&++engine_requests==fail_request;}
static void *engine_malloc(void *o,size_t n){(void)o;return fail_engine()?NULL:malloc(n);}
static void *engine_calloc(void *o,size_t n,size_t z){(void)o;return fail_engine()?NULL:calloc(n,z);}
static void engine_free(void *o,void *p){(void)o;free(p);}
static void *engine_realloc(void *o,void *p,size_t n){(void)o;return n&&fail_engine()?NULL:realloc(p,n);}
static size_t engine_usable(const void *p){return p?malloc_usable_size((void *)p):0;}
static JSValue arm(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)ctx;(void)self;(void)argc;(void)argv;engine_armed=true;engine_requests=0;return JS_UNDEFINED;
}
static unsigned array_oom_trial(bool prepared,unsigned nth){
    static const JSMallocFunctions mf={engine_calloc,engine_malloc,engine_free,engine_realloc,engine_usable};
    engine_armed=false;fail_request=nth;
    JSRuntime *rt=JS_NewRuntime2(&mf,NULL);CHECK(rt);JSContext *ctx=setup(rt);
    JSValue g=JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx,g,"arm",JS_NewCFunction(ctx,arm,"arm",0));JS_FreeValue(ctx,g);
    JSValue v=evaluate(ctx,"void pocket.kasane.procedural.register;arm();"
                          "pocket.kasane.procedural.register('derby.pt',[512]);",prepared);
    bool threw=JS_IsException(v);engine_armed=false;
    if(threw)JS_FreeValue(ctx,JS_GetException(ctx));else JS_FreeValue(ctx,v);
    unsigned result=(threw?1u:0u)|(allocations<<1);
    CHECK(!prepared_active&&!prepared_consuming);
    pocket_proc_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);return result;
}
static void array_oom_tests(void){
    unsigned failures=0,successes=0;
    for(unsigned i=1;i<=12;i++) {
        unsigned generic=array_oom_trial(false,i),prepared=array_oom_trial(true,i);
        CHECK(generic==prepared);
        if(generic==1)failures++;if(generic==2)successes++;
    }
    CHECK(failures&&successes);
}
int main(int argc,char **argv){
    CHECK(argc==2);FILE *f=fopen(argv[1],"rb");CHECK(f);
    char fixture[1024];size_t n=fread(fixture,1,sizeof fixture-1,f);fclose(f);fixture[n]=0;
    JSRuntime *rt=JS_NewRuntime();CHECK(rt);JSContext *ctx=setup(rt);
    ok(ctx,fixture,true);CHECK(prepared_hits==1&&allocations==1&&slots[0].rom);
    CHECK(((ksn_proc_rom_plan *)slots[0].plan)->args[0]==512);
    pocket_proc_reset();JS_FreeContext(ctx);ctx=setup(rt);
    /* Getter setup runs inside the same generated descriptor scope. */
    ok(ctx,"var effects=0;var h=pocket.kasane.procedural;var original=h.register;"
           "Object.defineProperty(h,'register',{configurable:true,get(){effects++;gc();return original;}});"
           "pocket.kasane.procedural.register('derby.pt',[512]);",true);
    CHECK(prepared_hits==0&&integer_eval(ctx,"effects")==1);
    ok(ctx,"Object.defineProperty(h,'register',{configurable:true,value:function(){effects++;return 7;}});",false);
    ok(ctx,source,true);CHECK(prepared_hits==0&&integer_eval(ctx,"effects")==2);
    ok(ctx,"Object.defineProperty(h,'register',{value:original});",false);
    /* A preexisting binding has no fresh rooted identity proof: generic. */
    ok(ctx,source,true);CHECK(prepared_hits==0);
    getter_and_oom(ctx);
    /* A registry replacement during callee evaluation invalidates the scope. */
    replacement=*(((ksn_proc_rom_plan *)slots[0].plan)->rom);
    ok(ctx,"Object.defineProperty(h,'register',{configurable:true,get(){replaceRegistry();return original;}});",false);
    unsigned before=prepared_hits;ok(ctx,source,true);CHECK(prepared_hits==before);
    pocket_proc_rom_plans(ksn_proc_rom_plans,ksn_proc_rom_plans_count);
    /* Descriptor is gone after throw, unsupported branching and nested funcs. */
    JSValue v=evaluate(ctx,"throw Error('stop');",true);CHECK(JS_IsException(v));
    JS_FreeValue(ctx,JS_GetException(ctx));CHECK(!prepared_active&&!prepared_consuming);
    ok(ctx,"if(effects>0)pocket.kasane.procedural.register('derby.pt',[512]);",true);
    CHECK(prepared_hits==before);
    pocket_proc_reset();JS_FreeContext(ctx);ctx=setup(rt);
    /* Early lookups must not root a function through later allocations. */
    ok(ctx,"var h=pocket.kasane.procedural;var f=h.register;"
           "h.register=function(){return 7;};f=null;gc();"
           "pocket.kasane.procedural.register('derby.pt',[512]);",true);
    CHECK(prepared_hits==0);
    pocket_proc_reset();JS_FreeContext(ctx);ctx=setup(rt);
    ok(ctx,"var h=pocket.kasane.procedural;var original=h.register;"
           "h.register=original.bind(null);"
           "pocket.kasane.procedural.register('derby.pt',[512]);",true);
    CHECK(prepared_hits==0&&allocations==1);
    pocket_proc_reset();JS_FreeContext(ctx);ctx=setup(rt);
    ok(ctx,"var h=pocket.kasane.procedural;var original=h.register;"
           "h.register=new Proxy(original,{});"
           "pocket.kasane.procedural.register('derby.pt',[512]);",true);
    CHECK(prepared_hits==0&&allocations==1);
    pocket_proc_reset();CHECK(!prepared_active);JS_FreeContext(ctx);JS_FreeRuntime(rt);
    root_lifetime_test();
    array_oom_tests();
    puts("PREPARED_RUNTIME PASS: generated startup consumer, identity/registry guards, GC, one-read fallback, native OOM, lifetime");
    return 0;
}
