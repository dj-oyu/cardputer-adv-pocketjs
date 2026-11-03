#include "pocket_shiki.h"
#include "pocket_api.h"
#include "pocket_proc.h"
#include "ksn_shiki.h"
#include "ksn_shiki_bench.h"
#include "ksn_shiki_jit.h"
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
 * {t, q, k, v} (type, Q, record size, the k * R values field by field as in
 * the case line, spec v1.4 sec.10.1) or a number j, the same memory as
 * region j. The values are laid out as planes of S = R rounded up to 8. It is the case line's DATA part as values;
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
                const uint32_t R=rs->well?(uint32_t)(n/kr):0,S=ksn_shiki_stride(R);
                void *mem=rs->well?calloc(ksn_shiki_region_elements(R,kr),t==KSN_SHIKI_I32?4:2):NULL;
                if(rs->well&&!mem)rs->well=false;
                rs->owned[k]=mem;
                for(int64_t v_at=0;mem&&v_at<n;v_at++){
                    const int64_t e=(v_at/R)*S+v_at%R;
                    JSValue x=JS_GetPropertyUint32(ctx,v,(uint32_t)v_at);
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
                rs->r[k]=(ksn_shiki_region){mem,R,(uint8_t)t,(uint8_t)q,(uint8_t)kr};
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
    ksn_shiki_plan *plan=calloc(1,KSN_SHIKI_PLAN_MAX);   /* jit NULL until register */
    ksn_proc_segment *seg=malloc(KSN_SHIKI_SEGMENTS*sizeof *seg);
    if(!plan||!seg){free(plan);free(seg);free_regions(rs);free(rs);return fail(ctx,POCKET_ERR_OUT_OF_MEMORY,"no room for a run");}
    ksn_shiki_vm vm={0};
    const bool ok=rs->well&&ksn_shiki_register(code,len,args,na,rs->r,rs->n,plan);
    const ksn_shiki_status s=ok?ksn_shiki_draw(plan,in,ni>KSN_SHIKI_INPUTS?KSN_SHIKI_INPUTS:ni,&vm,seg)
                               :KSN_SHIKI_INVALID;
    uint32_t h=2166136261u;
    ksn_shiki_result(ok,s,&vm,seg,fnv,&h);
    if(ok&&data)ksn_shiki_result_regions(rs->r,rs->n,fnv,&h);
    ksn_shiki_plan_dispose(plan);
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
    ksn_shiki_plan *plan=calloc(1,KSN_SHIKI_PLAN_MAX);
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
    if(plan)ksn_shiki_plan_dispose(plan);
    free(plan);free(seg);
    if(!ok||s!=KSN_SHIKI_DONE)snprintf(out,n,"CB shiki %s FAILED %d",c->name,ok?(int)s:-1);
    else snprintf(out,n,"CB shiki %s %ld %ld cy=%.2f",c->name,(long)c->r1,(long)c->r2,
                  (double)best/(COPIES*TURNS));
    return true;
}
/* After the op rows, the PIE prototype (ksn_shiki_bench.c,
 * docs/kasane/shiki-pie.md sec.6):
 *   - the checks: each program drawn on the C model and on PIE, every
 *     observable compared ("same" or the first difference);
 *   - the pairs, each a whole draw, the minimum of seven, with the vector
 *     instructions on the C model (pie=0) and on PIE (pie=1). Row "empty"
 *     is one SET: the fixed cost every draw includes. Scalar and emit rows
 *     do not use the switch, so they are timed once.
 * work is 16-B aligned (PIE's 128-bit accesses; an unaligned region takes
 * the C lane loops, which "check unaligned" covers). */
typedef struct {
    ksn_shiki_bench_case c;
    ksn_shiki_vm vm;
    ksn_shiki_plan *plan;
    ksn_proc_segment *seg;
    int16_t *work,*raw;
} bench_t;
static bench_t *bench_new(void){
    bench_t *b=calloc(1,sizeof *b);
    if(!b)return NULL;
    b->plan=calloc(1,KSN_SHIKI_PLAN_MAX);        /* jit NULL: dispose is safe before any register */
    b->seg=malloc(KSN_SHIKI_SEGMENTS*sizeof *b->seg);
    b->raw=malloc(KSN_SHIKI_BENCH_WORK*sizeof(int16_t)+16);
    if(!b->plan||!b->seg||!b->raw){free(b->plan);free(b->seg);free(b->raw);free(b);return NULL;}
    b->work=(int16_t *)(((uintptr_t)b->raw+15u)&~(uintptr_t)15u);
    return b;
}
static void bench_free(bench_t *b){
    if(b){if(b->plan)ksn_shiki_plan_dispose(b->plan);free(b->plan);free(b->seg);free(b->raw);free(b);}
}
/* What a draw leaves that the two paths must agree on, as one FNV hash:
 * status, the result line (steps, pen, registers, segments), v0..v7 and
 * the regions. */
static uint32_t observe(bench_t *b,ksn_shiki_status s){
    uint32_t h=2166136261u;
    ksn_shiki_result(true,s,&b->vm,b->seg,fnv,&h);
    fnv(&h,(const char *)(((uintptr_t)b->vm.vraw+15u)&~(uintptr_t)15u),128);
    ksn_shiki_result_regions(b->c.region,b->c.regions,fnv,&h);
    return h;
}
static bool check_line(unsigned i,char *out,size_t n){
    bench_t *b=bench_new();
    if(!b){snprintf(out,n,"CB shiki check %u NO MEMORY",i);return true;}
    /* Three runs of the same case: the C model, PIE instruction by
     * instruction, and PIE through the JIT's code (shiki-jit.md sec.3). */
    uint32_t h[3]={0,0,0};
    int16_t v3[3]={0,0,0};
    ksn_shiki_status st[3]={KSN_SHIKI_INVALID,KSN_SHIKI_INVALID,KSN_SHIKI_INVALID};
    bool ok=true,compiled=false;
    const bool kept=ksn_shiki_vec_pie,kept_jit=ksn_shiki_jit_enabled;
    for(unsigned run=0;ok&&run<3;run++){
        ksn_shiki_plan_dispose(b->plan);
        ok=ksn_shiki_bench_check_build(i,b->work,&b->c)&&
           ksn_shiki_register(b->c.code,b->c.len,NULL,0,b->c.region,b->c.regions,b->plan);
        if(!ok)break;
        if(run==2)compiled=b->plan->jit!=NULL;
        ksn_shiki_vec_pie=run>0;
        ksn_shiki_jit_enabled=run==2;
        st[run]=ksn_shiki_draw(b->plan,NULL,0,&b->vm,b->seg);
        ksn_shiki_vec_pie=kept;
        ksn_shiki_jit_enabled=kept_jit;
        h[run]=observe(b,st[run]);
        /* v1[0] (writeback reads it) for the log either way */
        v3[run]=((const int16_t *)(((uintptr_t)b->vm.vraw+15u)&~(uintptr_t)15u))[8];
    }
    if(!ok)snprintf(out,n,"CB shiki check %u FAILED to register",i);
    else snprintf(out,n,"CB shiki check %s %s model=%08lx/%d pie=%08lx/%d jit%s=%08lx/%d v1[0]=%d/%d/%d",
                  b->c.name,h[0]==h[1]&&h[1]==h[2]?"same":"DIFF",(unsigned long)h[0],(int)st[0],
                  (unsigned long)h[1],(int)st[1],compiled?"":"(none)",(unsigned long)h[2],(int)st[2],
                  v3[0],v3[1],v3[2]);
    bench_free(b);
    return true;
}
/* Row j of the timed part: 0 is "empty"; then each case, a vector case
 * twice (pie 0, 1). */
typedef struct { int index; bool pie,jit; } timed_row;
static unsigned timed_rows(timed_row *rows,unsigned max){
    unsigned n=0;
    if(n<max)rows[n++]=(timed_row){-1,false,false};
    for(unsigned i=0;i<ksn_shiki_bench_count();i++){
        if(n<max)rows[n++]=(timed_row){(int)i,false,false};
        if(i==1||i==5){                                               /* the vector forms */
            if(n<max)rows[n++]=(timed_row){(int)i,true,false};
            if(n<max)rows[n++]=(timed_row){(int)i,true,true};          /* and through the JIT */
        }
    }
    return n;
}
static bool pair_line(unsigned j,char *out,size_t n){
    timed_row rows[24];
    const unsigned count=timed_rows(rows,24);
    if(j>=count)return false;
    bench_t *b=bench_new();
    if(!b){snprintf(out,n,"CB shiki draw NO MEMORY");return true;}
    bool ok=true;
    if(rows[j].index<0){
        static const uint8_t empty[24]={'S','K','I','1',1,0,1,0,0,0,0,0,KSN_SHIKI_SET,0,0,0,1,0,0,0,0,0,0,0};
        b->c.name="empty";
        memcpy(b->c.code,empty,sizeof empty);
        b->c.len=sizeof empty;
    }else ok=ksn_shiki_bench_build((unsigned)rows[j].index,b->work,&b->c);
    ksn_shiki_plan_dispose(b->plan);
    ok=ok&&ksn_shiki_register(b->c.code,b->c.len,NULL,0,b->c.region,b->c.regions,b->plan);
    uint32_t best=UINT32_MAX;
    ksn_shiki_status s=KSN_SHIKI_INVALID;
    const bool kept=ksn_shiki_vec_pie,kept_jit=ksn_shiki_jit_enabled;
    ksn_shiki_vec_pie=rows[j].pie;
    ksn_shiki_jit_enabled=rows[j].jit;
    for(unsigned r=0;ok&&r<7;r++){
        const uint32_t t0=esp_cpu_get_cycle_count();
        s=ksn_shiki_draw(b->plan,NULL,0,&b->vm,b->seg);
        const uint32_t dt=esp_cpu_get_cycle_count()-t0;
        if(dt<best)best=dt;
    }
    ksn_shiki_vec_pie=kept;
    ksn_shiki_jit_enabled=kept_jit;
    if(!ok||s!=KSN_SHIKI_DONE)snprintf(out,n,"CB shiki draw %s FAILED %d",b->c.name?b->c.name:"?",ok?(int)s:-1);
    else snprintf(out,n,"CB shiki draw %s pie=%d%s cy=%lu us=%.1f steps=%u segs=%u",b->c.name,rows[j].pie?1:0,
                  rows[j].jit?(b->plan->jit?" jit":" jit(none)"):"",
                  (unsigned long)best,best/240.0,b->vm.steps,b->vm.count);
    bench_free(b);
    return true;
}
/* The scalar JIT on this CPU (shiki-jit.md sec.11): ksn_shiki_bench_random's
 * programs, the ones tools/shiki_jit/test_scalar.c runs on its emulator,
 * each drawn by the interpreter and by the plan's code, every observable
 * compared (as there: the step count and pen only for DONE, an error
 * inside a block counting to the block's end). Also the cycles per step of
 * both over the programs that finish. SCALAR_LINES lines of SCALAR_PER
 * programs, so that no call runs for long. */
#define SCALAR_LINES 4u
#define SCALAR_PER 200u
static uint32_t scalar_hash(const ksn_shiki_vm *vm,const ksn_proc_segment *seg,ksn_shiki_status s){
    uint32_t h=2166136261u;
    const int st=(int)s;
    fnv(&h,(const char *)&st,sizeof st);
    fnv(&h,(const char *)vm->reg,sizeof vm->reg);
    fnv(&h,(const char *)&vm->count,sizeof vm->count);
    fnv(&h,(const char *)seg,vm->count*sizeof *seg);
    if(s==KSN_SHIKI_DONE){
        const int16_t pen[3]={vm->pen_x,vm->pen_y,(int16_t)vm->pen_valid};
        const uint16_t more[3]={vm->steps,vm->raster,vm->depth};
        fnv(&h,(const char *)pen,sizeof pen);
        fnv(&h,(const char *)more,sizeof more);
    }
    return h;
}
static bool scalar_line(unsigned line,char *out,size_t n){
    bench_t *b=bench_new();
    if(!b){snprintf(out,n,"CB jit scalar NO MEMORY");return true;}
    uint32_t seed=1u+line*7919u;
    unsigned same=0,diff=0,none=0,first=UINT32_MAX,done=0;
    uint64_t cy[2]={0,0},steps=0;
    uint32_t bytes=0,inst=0;
    uint64_t compile_cy=0;                       /* compiling and copying to executable memory */
    const bool kept=ksn_shiki_jit_enabled;
    for(unsigned t=0;t<SCALAR_PER;t++){
        int32_t in[KSN_SHIKI_INPUTS];
        unsigned nin,index;
        ksn_shiki_bench_random(&seed,&b->c,in,&nin,&index);
        ksn_shiki_plan_dispose(b->plan);
        if(!ksn_shiki_register(b->c.code,b->c.len,NULL,0,NULL,0,b->plan)){t--;continue;}
        uint32_t h[2];
        ksn_shiki_status st[2];
        uint32_t dt[2];
        for(unsigned run=0;run<2;run++){
            ksn_shiki_jit_enabled=run==1;
            if(run==1){
                const uint32_t before=ksn_shiki_jit_bytes,c0=esp_cpu_get_cycle_count();
                if(!ksn_shiki_jit_compile_now(b->plan)){none++;break;}
                compile_cy+=esp_cpu_get_cycle_count()-c0;
                bytes+=ksn_shiki_jit_bytes-before;
                inst+=b->plan->count;
            }
            const uint32_t t0=esp_cpu_get_cycle_count();
            st[run]=ksn_shiki_draw_index(b->plan,in,nin,index,&b->vm,b->seg);
            dt[run]=esp_cpu_get_cycle_count()-t0;
            h[run]=scalar_hash(&b->vm,b->seg,st[run]);
        }
        ksn_shiki_jit_enabled=kept;
        if(!b->plan->jit)continue;
        if(h[0]==h[1])same++;
        else{diff++;if(first==UINT32_MAX)first=t;}
        if(st[0]==KSN_SHIKI_DONE){done++;cy[0]+=dt[0];cy[1]+=dt[1];steps+=b->vm.steps;}
    }
    ksn_shiki_jit_enabled=kept;
    snprintf(out,n,"CB jit scalar %u/%u %s same=%u diff=%u first=%d none=%u done=%u cy/step interp=%.1f jit=%.1f "
             "code=%.1f B/inst compile=%.1f cy/inst",line+1u,SCALAR_LINES,diff?"DIFF":"same",same,diff,first==UINT32_MAX?-1:(int)first,
             none,done,steps?(double)cy[0]/(double)steps:0.0,steps?(double)cy[1]/(double)steps:0.0,
             inst?(double)bytes/(double)inst:0.0,inst?(double)compile_cy/(double)inst:0.0);
    bench_free(b);
    return true;
}
/* Then the scalar JIT's lines; the last line: the JIT's stage-0 probe
 * (ksn_shiki_jit.c). */
static unsigned pair_count(void){
    timed_row rows[24];
    return ksn_shiki_bench_check_count()+timed_rows(rows,24)+SCALAR_LINES+1u;
}
#define PAIR_COUNT pair_count()
static JSValue js_bench(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    uint32_t i=0;
    if(argc<1||JS_ToUint32(ctx,&i,argv[0])!=0)
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,"shikiDebug.bench","expected (index)",
                                false,POCKET_OUTCOME_NOT_APPLIED);
    char line[224];
    const unsigned first=int_latency_count();
    if(i<first)return int_latency_case(i,line,sizeof line)?JS_NewString(ctx,line):JS_NULL;
    i-=first;
    if(i<OP_COUNT&&op_case_line(&OP_CASES[i],line,sizeof line))return JS_NewString(ctx,line);
    i-=(uint32_t)OP_COUNT;
    if(i<ksn_shiki_bench_check_count()&&check_line(i,line,sizeof line))return JS_NewString(ctx,line);
    i-=ksn_shiki_bench_check_count();
    if(pair_line(i,line,sizeof line))return JS_NewString(ctx,line);
    timed_row rows[24];
    i-=timed_rows(rows,24);
    if(i<SCALAR_LINES&&scalar_line(i,line,sizeof line))return JS_NewString(ctx,line);
    i-=SCALAR_LINES;
    if(i==0&&ksn_shiki_jit_probe(line,sizeof line))return JS_NewString(ctx,line);
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

/* cycles(): the CPU cycle counter (240 MHz, wraps every ~17.9 s), for timing
 * JS around native calls (apps/shikitest/draw_cost.js). */
static JSValue js_cycles(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argc;(void)argv;
    return JS_NewUint32(ctx,esp_cpu_get_cycle_count());
}
/* drawProfile(on): returns the stage sums so far ({n, parse, lookup, engine,
 * copy} in cycles, pocket_proc.h) and restarts them, on or off. */
static JSValue js_draw_profile(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    pocket_shiki_draw_prof *p=&pocket_shiki_prof;
    JSValue o=JS_NewObject(ctx);
    if(JS_IsException(o))return o;
    JS_SetPropertyStr(ctx,o,"n",JS_NewUint32(ctx,p->n));
    JS_SetPropertyStr(ctx,o,"parse",JS_NewUint32(ctx,p->parse));
    JS_SetPropertyStr(ctx,o,"lookup",JS_NewUint32(ctx,p->lookup));
    JS_SetPropertyStr(ctx,o,"engine",JS_NewUint32(ctx,p->engine));
    JS_SetPropertyStr(ctx,o,"copy",JS_NewUint32(ctx,p->copy));
    *p=(pocket_shiki_draw_prof){.on=argc>0&&JS_ToBool(ctx,argv[0])>0};
    return o;
}

esp_err_t pocket_shiki_debug_fill(JSContext *ctx,JSValueConst ns,void *unused){
    (void)unused;
    static const JSCFunctionListEntry methods[]={JS_CFUNC_DEF("run",4,js_run),JS_CFUNC_DEF("bench",1,js_bench),
        JS_CFUNC_DEF("benchCount",0,js_bench_count),
        JS_CFUNC_DEF("benchBuild",0,js_bench_build),
        JS_CFUNC_DEF("cycles",0,js_cycles),JS_CFUNC_DEF("drawProfile",1,js_draw_profile)};
    return JS_SetPropertyFunctionList(ctx,ns,methods,(int)(sizeof methods/sizeof methods[0]))<0?ESP_ERR_NO_MEM:ESP_OK;
}
/* No limits: the engine's are the spec's (sec.9.1); this surface adds none. */
static const pocket_capability_t capability={.name="kasane.shiki.debug",.supported=true,
    .available=true};
esp_err_t pocket_shiki_debug_install(JSContext *ctx,void *unused){
    (void)unused;
    esp_err_t err=pocket_api_register(&capability);
    return err==ESP_OK?pocket_api_lazy(ctx,"shikiDebug",pocket_shiki_debug_fill,NULL):err;
}
