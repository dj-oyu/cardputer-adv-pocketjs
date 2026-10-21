/* Host-only contract experiment. Include the actual adapter so the binding
 * identity, ROM registry, allocation path and result stay the production ones. */
#include <stddef.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bytecode.h"
static size_t allocation_bytes,allocation_count;
static void *tracked_malloc(size_t s){allocation_bytes=s;allocation_count++;return malloc(s);}
static void *tracked_calloc(size_t n,size_t s){allocation_bytes=n*s;allocation_count++;return calloc(n,s);}
#define malloc tracked_malloc
#define calloc tracked_calloc
#include "pocket_proc.c"
#undef calloc
#undef malloc

ksn_result pocket_kasane_proc_publish(void){return KSN_OK;}
ksn_result pocket_kasane_proc_publish_at(unsigned n,ksn_rect d){(void)n;(void)d;return KSN_OK;}
void pocket_kasane_invalidate(void){}
JSValue pocket_kasane_proc_resource(JSContext *c){(void)c;return JS_UNDEFINED;}
JSValue pocket_kasane_proc_resource_at(JSContext *c,unsigned n){(void)c;(void)n;return JS_UNDEFINED;}
JSValue pocket_api_throw(JSContext *c,const char *code,const char *op,const char *msg,bool retry,const char *out){
    (void)retry;(void)out;return JS_ThrowTypeError(c,"%s %s %s",code,op,msg);
}
_Static_assert(sizeof(float)==sizeof(uint32_t)&&FLT_RADIX==2&&FLT_MANT_DIG==24&&FLT_MAX_EXP==128,"prototype requires binary32");
static const ksn_proc_inst code[]={{.op=KSN_PROC_SET,.dst=0,.value=0}};
static const ksn_proc_patch patch[]={{.pc=0,.param=0,.field=KSN_PROC_FIELD_VALUE}};
static const ksn_proc_rom_entry entries[]={
    {.name="prep.zero",.code=code,.count=1},
    {.name="prep.two",.code=code,.count=1,.patch=patch,.patches=1,.params=2},
    {.name="prep.eight",.code=code,.count=1,.params=8},
};
static unsigned effects;
static JSValue side(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argc;(void)argv;effects++;
    JSValue a=JS_NewArray(ctx);JS_SetPropertyUint32(ctx,a,0,JS_NewInt32(ctx,1));
    JS_SetPropertyUint32(ctx,a,1,JS_NewInt32(ctx,2));return a;
}
static JSValue other(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)ctx;(void)self;(void)argc;(void)argv;effects++;return JS_UNDEFINED;
}
static JSValue api(JSContext *ctx){
    JSValue p=JS_NewObject(ctx),k=JS_NewObject(ctx);
    if(pocket_proc_install(ctx,k)!=ESP_OK)abort();
    JS_SetPropertyStr(ctx,p,"kasane",k);JSValue g=JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx,g,"pocket",p);JS_SetPropertyStr(ctx,g,"side",JS_NewCFunction(ctx,side,"side",0));
    JS_SetPropertyStr(ctx,g,"unknown",JS_NewCFunction(ctx,other,"unknown",0));JS_FreeValue(ctx,g);
    k=JS_GetPropertyStr(ctx,p,"kasane");JSValue proc=JS_GetPropertyStr(ctx,k,"procedural");
    JSValue fn=JS_GetPropertyStr(ctx,proc,"register");JS_FreeValue(ctx,proc);JS_FreeValue(ctx,k);return fn;
}
static bool binding_guard(JSContext *ctx,JSValueConst binding,JSValueConst captured){
    /* Callers must pass the callee already resolved by the ordinary VM,
     * after preserving callee/argument evaluation. Never look it up by name. */
    return prep_native_identity(ctx,binding,js_register)&&
           JS_VALUE_GET_TAG(captured)==JS_TAG_OBJECT&&
           JS_VALUE_GET_PTR(binding)==JS_VALUE_GET_PTR(captured)&&
           prep_native_identity(ctx,captured,js_register)&&rom_table==entries&&
           rom_count==sizeof entries/sizeof entries[0];
}
static const char *layout(const prep_call *c,const ksn_proc_rom_entry **entry){
    if(!c->path_candidate)return "unknown-call-or-transform";
    if(c->argc<1||c->argc>3)return "arity";
    if(c->args[0].kind!=P_STRING)return "dynamic-name";
    const ksn_proc_rom_entry *rom=NULL;
    for(unsigned i=0;i<rom_count;i++)if(!strcmp(rom_table[i].name,c->args[0].u.text))rom=&rom_table[i];
    if(!rom||!ksn_proc_rom_entry_valid(rom))return "unknown-contract";
    unsigned n=c->argc>=2&&c->args[1].kind==P_VECTOR?c->args[1].length:0;
    if(c->argc>=2&&c->args[1].kind!=P_VECTOR&&c->args[1].kind!=P_EMPTY)return "dynamic-input";
    if(n!=rom->params)return "argument-count";
    if(c->argc==3&&c->args[2].kind!=P_EMPTY)return "points-need-generic-contract";
    /* Bounded reusable analysis scratch, not an additional runtime pool.
     * Write float fields directly, not through a JSValue/argv staging array. */
    union {max_align_t alignment;unsigned char data[sizeof(ksn_proc_rom_plan)+PREP_NUMBERS*sizeof(float)];} w;
    memset(&w,0,sizeof w);ksn_proc_rom_plan *p=(void *)w.data;p->params=(uint8_t)n;
    for(unsigned i=0;i<n;i++){
        double v=c->args[1].u.numbers[i];if(!isfinite(v)||!isfinite((float)v))return "number-domain";
        for(unsigned k=0;k<rom->patches;k++)if(rom->patch[k].param==i){
            unsigned f=rom->patch[k].field;
            if((f==KSN_PROC_FIELD_A&&(v<0||v>UINT8_MAX||v!=floor(v)))||
               (f==KSN_PROC_FIELD_COLOR&&(v<0||v>UINT16_MAX||v!=floor(v))))return "field-domain";
        }
        p->args[i]=(float)v;
    }
    if(!ksn_proc_rom_plan_prepare(p,rom))return "invalid-program";
    *entry=rom;return NULL;
}
static void require(bool x){if(!x){fprintf(stderr,"prepare contract assertion failed\n");exit(1);}}
static void identity_tests(JSContext *ctx,JSValueConst fn){
    require(binding_guard(ctx,fn,fn));
    JSValue global=JS_GetGlobalObject(ctx),p=JS_GetPropertyStr(ctx,global,"pocket");
    JSValue k=JS_GetPropertyStr(ctx,p,"kasane"),proc=JS_GetPropertyStr(ctx,k,"procedural");
    JSValue fake=JS_NewCFunction(ctx,other,"register",1);
    JS_SetPropertyStr(ctx,proc,"register",JS_DupValue(ctx,fake));
    JS_RunGC(JS_GetRuntime(ctx)); /* fn is now held by the C binding, not that property. */
    require(binding_guard(ctx,fn,fn)&&!binding_guard(ctx,fn,fake));
    JS_SetPropertyStr(ctx,proc,"register",JS_DupValue(ctx,fn));
    require(!binding_guard(ctx,fn,fake)); /* A later restore cannot validate a captured replacement. */
    JS_FreeValue(ctx,fake);JS_FreeValue(ctx,proc);JS_FreeValue(ctx,k);
    JS_FreeValue(ctx,p);JS_FreeValue(ctx,global);
    /* Same native entry and name, but a different object is not the rooted
     * binding. In particular do not reread a restored namespace property. */
    fake=JS_NewCFunction(ctx,js_register,"register",1);
    require(!binding_guard(ctx,fn,fake));JS_FreeValue(ctx,fake);
    JSContext *foreign=JS_NewContext(JS_GetRuntime(ctx));require(foreign);
    fake=JS_NewCFunction(foreign,js_register,"register",1);require(!binding_guard(ctx,fn,fake));
    JS_FreeValue(foreign,fake);JS_FreeContext(foreign);
    const ksn_proc_rom_entry *saved=rom_table;rom_table=NULL;require(!binding_guard(ctx,fn,fn));rom_table=saved;
    require(binding_guard(ctx,fn,fn));
}
int main(int argc,char **argv){
    if(argc<2||argc>4)return 2;
    bool exercise=argc>=3&&!strcmp(argv[2],"--exercise");
    bool compare=argc==4&&!strcmp(argv[3],"--compare-plan");
    FILE *f=fopen(argv[1],"rb");if(!f)return 2;
    char source[16385];size_t n=fread(source,1,sizeof source,f);fclose(f);
    if(!n||n==sizeof source){puts("{\"bounded_fallback\":true,\"calls\":[]}");return 0;}
    source[n]=0;JSRuntime *rt=JS_NewRuntime();if(!rt)return 2;JS_SetMemoryLimit(rt,16*1024*1024);
    JSContext *ctx=JS_NewContext(rt);require(ctx!=NULL);pocket_proc_rom_plans(entries,sizeof entries/sizeof entries[0]);
    JSValue binding=api(ctx);identity_tests(ctx,binding);
    JSValue compiled=JS_Eval(ctx,source,n,argv[1],JS_EVAL_TYPE_GLOBAL|JS_EVAL_FLAG_COMPILE_ONLY);
    if(JS_IsException(compiled)){
        JS_FreeValue(ctx,JS_GetException(ctx));JS_FreeValue(ctx,binding);JS_FreeContext(ctx);JS_FreeRuntime(rt);
        puts("{\"compile_failed\":true,\"calls\":[]}");return 2;
    }
    prep_report report;prep_analyze(ctx,compiled,&report);require(effects==0&&allocation_count==0);
    printf("{\"runtime_connected\":false,\"default_route\":\"generic\",\"pointer_bytes\":%zu,\"jsvalue_bytes\":%zu,\"abstract_scratch_bytes\":%zu,\"bounded_fallback\":%s,\"identity_tests\":true,\"preparation_effects\":%u,\"calls\":[",sizeof(void *),sizeof(JSValue),report.scratch_bytes,report.bounded_fallback?"true":"false",effects);
    unsigned planned=0;size_t last_expected=0;
    for(unsigned i=0;i<report.count;i++){
        prep_call *c=&report.calls[i];const ksn_proc_rom_entry *rom=NULL;const char *why=layout(c,&rom);
        printf("%s{\"function\":%u,\"pc\":%u,\"argc\":%u,\"route\":\"%s\"",i?",":"",c->function,c->pc,c->argc,why?"generic":"guarded-candidate");
        if(why)printf(",\"reason\":\"%s\"",why);
        else{
            planned++;last_expected=ksn_proc_rom_plan_bytes(rom->params);
            printf(",\"guard\":\"captured-callee-and-registry-identity\",\"params\":%u,\"minimum_bytes\":%zu,\"alignment\":%zu,\"args_offset\":%zu,\"element_bytes\":%zu,\"sizeof_floor\":%zu",rom->params,last_expected,_Alignof(ksn_proc_rom_plan),offsetof(ksn_proc_rom_plan,args),sizeof(float),sizeof(ksn_proc_rom_plan));
            printf(",\"element_alignment\":%zu,\"literal_float32_bits\":[",_Alignof(float));
            for(unsigned k=0;k<rom->params;k++){
                float v=(float)c->args[1].u.numbers[k];uint32_t bits;
                memcpy(&bits,&v,sizeof bits);printf("%s\"%08x\"",k?",":"",(unsigned)bits);
            }
            putchar(']');
        }
        putchar('}');
    }
    bool threw=false;
    if(exercise){
        JSValue value=JS_EvalFunction(ctx,JS_DupValue(ctx,compiled));threw=JS_IsException(value);
        if(threw)JS_FreeValue(ctx,JS_GetException(ctx));else JS_FreeValue(ctx,value);
        /* This exercises the unchanged generic path, not a specialized VM
         * dispatcher. One-call fixtures compare its actual allocation. */
        if(compare){
            require(planned==1&&allocation_count==1&&!threw);
            require(allocation_bytes==last_expected&&slots[0].rom);
            const ksn_proc_rom_plan *actual=slots[0].plan;
            for(unsigned k=0;k<report.count;k++){
                const ksn_proc_rom_entry *rom=NULL;
                if(layout(&report.calls[k],&rom))continue;
                require(actual->rom==rom&&actual->params==rom->params&&actual->valid);
                for(unsigned i=0;i<rom->params;i++){
                    float expected=(float)report.calls[k].args[1].u.numbers[i];
                    require(!memcmp(&actual->args[i],&expected,sizeof expected));
                }
            }
        }
    }
    printf("],\"generic_exercised\":%s,\"generic_threw\":%s,\"runtime_effects\":%u,\"native_allocations\":%zu,\"last_allocation_bytes\":%zu}\n",exercise?"true":"false",threw?"true":"false",effects,allocation_count,allocation_bytes);
    JS_FreeValue(ctx,compiled);JS_FreeValue(ctx,binding);pocket_proc_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);return 0;
}
