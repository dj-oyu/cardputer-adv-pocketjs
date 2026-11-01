#include "pocket_shiki.h"
#include "pocket_api.h"
#include "ksn_shiki.h"
#include "ksn_shiki_bench.h"
#include "int_latency.h"
#include "esp_cpu.h"
#include "esp_app_desc.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* run(code, args, inputs[, data]): registers code (a Uint8Array in the sec.3
 * byte format) with args and data regions, draws once with inputs, and
 * returns "<hash> <status>":
 * the FNV-1a 32 of the shared text contract's result line (ksn_shiki_result,
 * the line tools/kasane_contract/shiki_run.c prints) in 8 hex digits, and
 * DONE / INVALID / LIMIT / BADINPUT / REJECT. The line itself can be 30 KB
 * (1,024 segments), so the device hashes it in pieces instead of building a
 * guest string; the host (tools/kasane_ir/shiki_device.mjs) hashes the line
 * its own engines print and compares.
 *
 *
 * data (v1.3, apps/shikitest/shiki_data_test.js) is an array of regions, each
 * {t, q, k, v} (type, Q, record size, the elements as numbers) or a number j,
 * the same memory as region j. It is the case line's DATA part as values;
 * the result line then carries the regions' contents (" D ..."), and a
 * malformed region makes the case REJECT, as in shiki_run.c.
 *
 * The plan, the segments (about 11 KB) and the regions are allocated per
 * call and freed before returning: the surface exists only in the test app's
 * session, and nothing of it may sit in every build's .bss. */

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
/* An integer property, or false. */
static bool int_prop(JSContext *ctx,JSValueConst o,const char *name,int64_t *out){
    JSValue v=JS_GetPropertyStr(ctx,o,name);
    const bool ok=JS_IsNumber(v)&&JS_ToInt64(ctx,out,v)==0;
    JS_FreeValue(ctx,v);
    return ok;
}
#define MAX_REGIONS (KSN_SHIKI_REGIONS+4)
typedef struct { ksn_shiki_region r[MAX_REGIONS]; void *owned[MAX_REGIONS]; unsigned n; bool well; } regions_t;
/* Reads data into rs: false only if data is not an array of regions at all
 * (the caller's mistake); a region shiki.data() would refuse sets well = false. */
static bool read_regions(JSContext *ctx,JSValueConst data,regions_t *rs){
    memset(rs,0,sizeof *rs);
    rs->well=true;
    int64_t count=0;
    if(!JS_IsArray(data)||!int_prop(ctx,data,"length",&count)||count<0||count>MAX_REGIONS)return false;
    rs->n=(unsigned)count;
    for(unsigned k=0;k<rs->n;k++){
        JSValue g=JS_GetPropertyUint32(ctx,data,k);
        bool ok=true;
        if(JS_IsNumber(g)){
            int64_t j=-1;
            JS_ToInt64(ctx,&j,g);
            if(j<0||j>=(int64_t)k)rs->well=false;
            else rs->r[k]=rs->r[j];
        }else{
            int64_t t=-1,q=-1,kr=0,n=-1;
            JSValue v=JS_GetPropertyStr(ctx,g,"v");
            ok=int_prop(ctx,g,"t",&t)&&int_prop(ctx,g,"q",&q)&&int_prop(ctx,g,"k",&kr)&&
               JS_IsArray(v)&&int_prop(ctx,v,"length",&n);
            if(ok){
                if(t<0||t>2||q<0||q>30||kr<1||kr>8||n<kr||n%kr||n/kr>KSN_SHIKI_RECORDS)rs->well=false;
                void *mem=rs->well?calloc((size_t)n,t==KSN_SHIKI_I32?4:2):NULL;
                if(rs->well&&!mem)rs->well=false;
                rs->owned[k]=mem;
                for(int64_t e=0;mem&&e<n;e++){
                    JSValue x=JS_GetPropertyUint32(ctx,v,(uint32_t)e);
                    int64_t val=0;
                    const bool num=JS_IsNumber(x)&&JS_ToInt64(ctx,&val,x)==0;
                    JS_FreeValue(ctx,x);
                    const bool in=num&&(t==0?val>=-32768&&val<=32767:t==1?val>=0&&val<=65535
                                                :val>=INT32_MIN&&val<=INT32_MAX);
                    if(!in){rs->well=false;break;}
                    if(t==0)((int16_t *)mem)[e]=(int16_t)val;
                    else if(t==1)((uint16_t *)mem)[e]=(uint16_t)val;
                    else ((int32_t *)mem)[e]=(int32_t)val;
                }
                rs->r[k]=(ksn_shiki_region){mem,(uint32_t)n,(uint8_t)t,(uint8_t)q,(uint8_t)kr};
            }
            JS_FreeValue(ctx,v);
        }
        JS_FreeValue(ctx,g);
        if(!ok)return false;
    }
    return true;
}
static void free_regions(regions_t *rs){
    for(unsigned k=0;k<MAX_REGIONS;k++)free(rs->owned[k]);
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
        return fail(ctx,POCKET_ERR_INVALID_ARGUMENT,"expected (Uint8Array, number[], number[][, data])");
    }
    const bool data=argc>3&&!JS_IsUndefined(argv[3]);
    regions_t *rs=calloc(1,sizeof *rs);
    if(!rs)return fail(ctx,POCKET_ERR_OUT_OF_MEMORY,"no room for a run");
    rs->well=true;                                 /* no data: no region to refuse */
    if(data&&!read_regions(ctx,argv[3],rs)){
        free_regions(rs);free(rs);
        return fail(ctx,POCKET_ERR_INVALID_ARGUMENT,"expected data as [{t, q, k, v} | index, ...]");
    }
    ksn_shiki_plan *plan=malloc(KSN_SHIKI_PLAN_MAX);
    ksn_proc_segment *seg=malloc(KSN_SHIKI_SEGMENTS*sizeof *seg);
    if(!plan||!seg){free(plan);free(seg);free_regions(rs);free(rs);return fail(ctx,POCKET_ERR_OUT_OF_MEMORY,"no room for a run");}
    ksn_shiki_vm vm={0};
    const bool ok=rs->well&&ksn_shiki_register(code,len,args,na,rs->r,rs->n,plan);
    const ksn_shiki_status s=ok?ksn_shiki_draw(plan,in,ni>KSN_SHIKI_INPUTS?KSN_SHIKI_INPUTS:ni,&vm,seg)
                               :KSN_SHIKI_INVALID;
    uint32_t h=2166136261u;
    ksn_shiki_result(ok,s,&vm,seg,fnv,&h);
    if(ok&&data)ksn_shiki_result_regions(rs->r,rs->n,fnv,&h);
    free(plan);free(seg);free_regions(rs);free(rs);
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
    ksn_shiki_plan *plan=malloc(KSN_SHIKI_PLAN_MAX);
    ksn_proc_segment *seg=malloc(KSN_SHIKI_SEGMENTS*sizeof *seg);
    bool ok=plan&&seg&&ksn_shiki_register(code,sizeof code,NULL,0,NULL,0,plan);
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
/* After the op rows: the PIE prototype's pairs (ksn_shiki_bench.c,
 * docs/kasane/shiki-pie.md sec.6), each a whole draw, the minimum of seven,
 * with the segments' count and steps so a pair's rows can be checked equal.
 * Row "empty" is one SET: the draw's fixed cost (the vm's reset and the
 * input conversion) that every other row includes. */
static bool pair_line(unsigned i,char *out,size_t n){
    ksn_shiki_bench_case *c=malloc(sizeof *c);
    int16_t *work=malloc(KSN_SHIKI_BENCH_WORK*sizeof *work);
    ksn_shiki_plan *plan=malloc(KSN_SHIKI_PLAN_MAX);
    ksn_proc_segment *seg=malloc(KSN_SHIKI_SEGMENTS*sizeof *seg);
    ksn_shiki_vm *vm=malloc(sizeof *vm);
    bool ok=c&&work&&plan&&seg&&vm;
    if(ok&&i==0){
        static const uint8_t empty[24]={'S','K','I','1',1,0,1,0,0,0,0,0,KSN_SHIKI_SET,0,0,0,1,0,0,0,0,0,0,0};
        memset(c,0,sizeof *c);
        c->name="empty";
        memcpy(c->code,empty,sizeof empty);
        c->len=sizeof empty;
    }else if(ok)ok=ksn_shiki_bench_build(i-1,work,c);
    ok=ok&&ksn_shiki_register(c->code,c->len,NULL,0,c->region,c->regions,plan);
    uint32_t best=UINT32_MAX;
    ksn_shiki_status s=KSN_SHIKI_INVALID;
    for(unsigned r=0;ok&&r<7;r++){
        const uint32_t t0=esp_cpu_get_cycle_count();
        s=ksn_shiki_draw(plan,NULL,0,vm,seg);
        const uint32_t dt=esp_cpu_get_cycle_count()-t0;
        if(dt<best)best=dt;
    }
    if(!ok||s!=KSN_SHIKI_DONE)snprintf(out,n,"CB shiki draw %s FAILED %d",c?c->name:"?",ok?(int)s:-1);
    else snprintf(out,n,"CB shiki draw %s cy=%lu us=%.1f steps=%u segs=%u",c->name,(unsigned long)best,
                  best/240.0,vm->steps,vm->count);
    free(c);free(work);free(plan);free(seg);free(vm);
    return true;
}
#define PAIR_COUNT (1+ksn_shiki_bench_count())
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
    i-=(uint32_t)OP_COUNT;
    if(i<PAIR_COUNT&&pair_line(i,line,sizeof line))return JS_NewString(ctx,line);
    return JS_NULL;
}

/* benchCount(): how many lines bench() gives, so the app can show progress. */
static JSValue js_bench_count(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argc;(void)argv;
    return JS_NewUint32(ctx,(uint32_t)(int_latency_count()+OP_COUNT+PAIR_COUNT));
}

/* benchBuild(): "<version> <first 8 bytes of the ELF's SHA-256 in hex>". The
 * Shiki rows move between builds (the engine's layout), so results are
 * compared only between runs that name the same build
 * (docs/perf/int-latency.md). */
static JSValue js_bench_build(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argc;(void)argv;
    const esp_app_desc_t *d=esp_app_get_description();
    char out[80];
    int n=snprintf(out,sizeof out,"%.32s ",d->version);
    for(unsigned k=0;k<8&&n>0&&(size_t)n+2<sizeof out;k++)n+=snprintf(out+n,sizeof out-(size_t)n,"%02x",d->app_elf_sha256[k]);
    return JS_NewString(ctx,out);
}

esp_err_t pocket_shiki_debug_fill(JSContext *ctx,JSValueConst ns,void *unused){
    (void)unused;
    static const JSCFunctionListEntry methods[]={JS_CFUNC_DEF("run",4,js_run),JS_CFUNC_DEF("bench",1,js_bench),
        JS_CFUNC_DEF("benchCount",0,js_bench_count),
        JS_CFUNC_DEF("benchBuild",0,js_bench_build)};
    return JS_SetPropertyFunctionList(ctx,ns,methods,4)<0?ESP_ERR_NO_MEM:ESP_OK;
}
/* No limits: the engine's are the spec's (sec.9.1); this surface adds none. */
static const pocket_capability_t capability={.name="kasane.shiki.debug",.supported=true,
    .available=true};
esp_err_t pocket_shiki_debug_install(JSContext *ctx,void *unused){
    (void)unused;
    esp_err_t err=pocket_api_register(&capability);
    return err==ESP_OK?pocket_api_lazy(ctx,"shikiDebug",pocket_shiki_debug_fill,NULL):err;
}
