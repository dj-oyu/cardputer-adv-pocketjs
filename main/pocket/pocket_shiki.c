#include "pocket_shiki.h"
#include "pocket_api.h"
#include "ksn_shiki.h"
#include "int_latency.h"
#include "esp_cpu.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* run(code, args, inputs): registers code (a Uint8Array in the sec.3 byte
 * format) with args, draws once with inputs, and returns "<hash> <status>":
 * the FNV-1a 32 of the shared text contract's result line (ksn_shiki_result,
 * the line tools/kasane_contract/shiki_run.c prints) in 8 hex digits, and
 * DONE / INVALID / LIMIT / BADINPUT / REJECT. The line itself can be 30 KB
 * (1,024 segments), so the device hashes it in pieces instead of building a
 * guest string; the host (tools/kasane_ir/shiki_device.mjs) hashes the line
 * its own engines print and compares.
 *
 * The plan and the segments (about 11 KB) are allocated per call and freed
 * before returning: the surface exists only in the test app's session, and
 * nothing of it may sit in every build's .bss. */

#define MAX_NUMBERS 64   /* as shiki_run.c: extra inputs reach the engine, which ignores them */

static JSValue fail(JSContext *ctx,const char *code,const char *message){
    return pocket_api_throw(ctx,code,"shikiDebug.run",message,false,POCKET_OUTCOME_NOT_APPLIED);
}
/* An array of at most MAX_NUMBERS numbers. Not coerced: the test is about
 * what JS numbers become (sec.2.4), so a non-number is the caller's mistake. */
static bool numbers(JSContext *ctx,JSValueConst v,double *out,unsigned *n){
    JSValue len=JS_GetPropertyStr(ctx,v,"length");
    uint32_t count=0;
    const bool ok=JS_IsArray(v)&&!JS_IsException(len)&&JS_ToUint32(ctx,&count,len)==0&&count<=MAX_NUMBERS;
    JS_FreeValue(ctx,len);
    if(!ok)return false;
    for(uint32_t k=0;k<count;k++){
        JSValue e=JS_GetPropertyUint32(ctx,v,k);
        const bool good=JS_IsNumber(e)&&JS_ToFloat64(ctx,&out[k],e)==0;
        JS_FreeValue(ctx,e);
        if(!good)return false;
    }
    *n=count;
    return true;
}
static void fnv(void *ctx,const char *text,size_t len){
    uint32_t *h=ctx;
    for(size_t k=0;k<len;k++){*h^=(uint8_t)text[k];*h*=16777619u;}
}
static JSValue js_run(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    size_t len=0;
    uint8_t *code=argc>0?JS_GetUint8Array(ctx,&len,argv[0]):NULL;
    double args[MAX_NUMBERS],in[MAX_NUMBERS];
    unsigned na=0,ni=0;
    if(!code||argc<3||!numbers(ctx,argv[1],args,&na)||!numbers(ctx,argv[2],in,&ni)){
        JS_FreeValue(ctx,JS_GetException(ctx));   /* a non-Uint8Array leaves one pending */
        return fail(ctx,POCKET_ERR_INVALID_ARGUMENT,"expected (Uint8Array, number[], number[])");
    }
    ksn_shiki_plan *plan=malloc(sizeof *plan);
    ksn_proc_segment *seg=malloc(KSN_SHIKI_SEGMENTS*sizeof *seg);
    if(!plan||!seg){free(plan);free(seg);return fail(ctx,POCKET_ERR_OUT_OF_MEMORY,"no room for a run");}
    ksn_shiki_vm vm={0};
    const bool ok=ksn_shiki_register(code,len,args,na,plan);
    const ksn_shiki_status s=ok?ksn_shiki_draw(plan,in,ni>KSN_SHIKI_INPUTS?KSN_SHIKI_INPUTS:ni,&vm,seg)
                               :KSN_SHIKI_INVALID;
    uint32_t h=2166136261u;
    ksn_shiki_result(ok,s,&vm,seg,fnv,&h);
    free(plan);free(seg);
    static const char *const name[]={"RUNNING","DONE","INVALID","LIMIT","BADINPUT"};
    char out[32];
    snprintf(out,sizeof out,"%08lx %s",(unsigned long)h,ok?name[s]:"REJECT");
    return JS_NewString(ctx,out);
}

/* bench(i): one line of cycle counts, or null past the last case (the test
 * app apps/shikitest/cycle_bench.js, docs/perf/int-latency.md). Cases 0.. are
 * the instructions (main/hal/int_latency.c); after them, what one Shiki
 * instruction costs inside ksn_shiki_draw: a program that sets r1, r2 and
 * runs REPEAT 200 over sixteen copies of the instruction (r3 = op(r1, r2)),
 * timed whole, the minimum of seven draws, divided by the 3,200 copies. The
 * SET row is the floor (dispatch and the loop's share) to read the rest
 * against. Values stay put (r1, r2 are never written), so an operand class is
 * the same for every copy. */
typedef struct { const char *name; uint8_t op,a,b; int32_t imm; uint16_t aux; int32_t r1,r2; } op_case;
static const op_case OP_CASES[]={
    {"SET",KSN_SHIKI_SET,0,0,0,0,5,7},
    {"ADD",KSN_SHIKI_ADD,1,2,0,0,5,7},
    {"ADD sat",KSN_SHIKI_ADD,1,2,0,0,0x7fffffff,0x7fffffff},
    {"SUB",KSN_SHIKI_SUB,1,2,0,0,5,7},
    {"NEG",KSN_SHIKI_NEG,1,0,0,0,5,7},
    {"MUL s16 small",KSN_SHIKI_MUL,1,2,0,16,5<<16,7<<16},
    {"MUL s16 large",KSN_SHIKI_MUL,1,2,0,16,0x12345678,0x7654321},
    {"MUL s0 sat",KSN_SHIKI_MUL,1,2,0,0,0x7fffffff,0x7fffffff},
    {"MUL s40",KSN_SHIKI_MUL,1,2,0,40,0x12345678,0x7654321},
    {"SHIFT +5",KSN_SHIKI_SHIFT,1,0,5,0,0x12345,7},
    {"SHIFT -5",KSN_SHIKI_SHIFT,1,0,-5,0,0x12345,7},
    {"SIN",KSN_SHIKI_SIN,1,0,0,0,12345,7},
    {"ADD.H",KSN_SHIKI_ADD_H,1,2,0,0,1000,2000},
    {"MUL.H s8",KSN_SHIKI_MUL_H,1,2,0,8,1000,2000},
    {"SHIFT.H +3",KSN_SHIKI_SHIFT_H,1,0,3,0,1000,7},
};
#define OP_COUNT (sizeof OP_CASES/sizeof OP_CASES[0])
static void put_inst(uint8_t *b,uint8_t op,uint8_t dst,uint8_t a,uint8_t bb,int32_t imm,uint16_t aux){
    const uint32_t u=(uint32_t)imm;
    const uint8_t w[12]={op,dst,a,bb,(uint8_t)u,(uint8_t)(u>>8),(uint8_t)(u>>16),(uint8_t)(u>>24),
                         (uint8_t)aux,(uint8_t)(aux>>8),0,0};
    memcpy(b,w,sizeof w);
}
static bool op_case_line(const op_case *c,char *out,size_t n){
    enum { COPIES=16, TURNS=200, COUNT=COPIES+4 };
    uint8_t code[12+12*COUNT]={'S','K','I','1',1,0,COUNT,0,0,0,0,0};
    uint8_t *p=code+12;
    put_inst(p,KSN_SHIKI_SET,1,0,0,c->r1,0);p+=12;
    put_inst(p,KSN_SHIKI_SET,2,0,0,c->r2,0);p+=12;
    put_inst(p,KSN_SHIKI_REPEAT,0,0,0,TURNS,0);p+=12;
    for(unsigned k=0;k<COPIES;k++){put_inst(p,c->op,3,c->a,c->b,c->imm,c->aux);p+=12;}
    put_inst(p,KSN_SHIKI_END,0,0,0,0,0);
    ksn_shiki_plan *plan=malloc(sizeof *plan);
    ksn_proc_segment *seg=malloc(KSN_SHIKI_SEGMENTS*sizeof *seg);
    bool ok=plan&&seg&&ksn_shiki_register(code,sizeof code,NULL,0,plan);
    uint32_t best=UINT32_MAX;
    ksn_shiki_status s=KSN_SHIKI_INVALID;
    for(unsigned r=0;ok&&r<7;r++){
        ksn_shiki_vm vm;
        const uint32_t t0=esp_cpu_get_cycle_count();
        s=ksn_shiki_draw(plan,NULL,0,&vm,seg);
        const uint32_t dt=esp_cpu_get_cycle_count()-t0;
        if(dt<best)best=dt;
    }
    free(plan);free(seg);
    if(!ok||s!=KSN_SHIKI_DONE)snprintf(out,n,"CB shiki %s FAILED %d",c->name,ok?(int)s:-1);
    else snprintf(out,n,"CB shiki %s %ld %ld cy=%.2f",c->name,(long)c->r1,(long)c->r2,
                  (double)best/(COPIES*TURNS));
    return true;
}
static JSValue js_bench(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    uint32_t i=0;
    if(argc<1||JS_ToUint32(ctx,&i,argv[0])!=0)
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,"shikiDebug.bench","expected (index)",
                                false,POCKET_OUTCOME_NOT_APPLIED);
    char line[160];
    const unsigned first=int_latency_count();
    if(i<first)return int_latency_case(i,line,sizeof line)?JS_NewString(ctx,line):JS_NULL;
    i-=first;
    if(i<OP_COUNT&&op_case_line(&OP_CASES[i],line,sizeof line))return JS_NewString(ctx,line);
    return JS_NULL;
}

esp_err_t pocket_shiki_debug_fill(JSContext *ctx,JSValueConst ns,void *unused){
    (void)unused;
    static const JSCFunctionListEntry methods[]={JS_CFUNC_DEF("run",3,js_run),JS_CFUNC_DEF("bench",1,js_bench)};
    return JS_SetPropertyFunctionList(ctx,ns,methods,2)<0?ESP_ERR_NO_MEM:ESP_OK;
}
/* No limits: the engine's are the spec's (sec.9.1); this surface adds none. */
static const pocket_capability_t capability={.name="kasane.shiki.debug",.supported=true,
    .available=true};
esp_err_t pocket_shiki_debug_install(JSContext *ctx,void *unused){
    (void)unused;
    esp_err_t err=pocket_api_register(&capability);
    return err==ESP_OK?pocket_api_lazy(ctx,"shikiDebug",pocket_shiki_debug_fill,NULL):err;
}
