/* Built-in (flash) plans registered by name, against the same plans
 * registered as arrays, on the real QuickJS and pocket_proc.c
 * (docs/kasane/flash-plan.md). Built by run_pocket_proc_rom_qjs.py under
 * ASan/UBSan with proc_alloc_hook.h force-included.
 *
 * pocket_proc.c is included, not linked: the comparison reads the adapter's
 * own VM, scratch frame and slots after each draw, so "the same output" is
 * the VM's state, every segment (ends and colour, in order) and the raster
 * steps, not the pixels that happen to land on the panel.
 *
 * argv: the table's app JS as the firmware ships it (lower_plans.mjs, the
 * packed plans and their decoder), and the captured DERBY cases
 * (tools/kasane_ir/derby_plans.mjs --cases: plan, arguments, inputs). */
#include "pocket_proc.c"
#include "quickjs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(x) do { if(!(x)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);} } while(0)

extern const ksn_proc_rom_entry ksn_proc_rom_plans[];
extern const unsigned ksn_proc_rom_plans_count;

/* Linked with --wrap=ksn_proc_run: a plan's run falls back to the VM's
 * reference loop only when the VM does not hold that plan's program (or the
 * plan is invalid). Output is identical either way, so equal frames alone
 * would not show that a named plan takes its fused path; this count does. */
static unsigned fallbacks;
ksn_proc_status __real_ksn_proc_run(ksn_proc_vm *vm);
ksn_proc_status __wrap_ksn_proc_run(ksn_proc_vm *vm){fallbacks++;return __real_ksn_proc_run(vm);}
static bool fail_alloc,fail_malloc;
static size_t callocs[8];
static unsigned calloc_n;
void *proc_test_malloc(size_t size){
    if(fail_alloc||fail_malloc)return NULL;
    return (malloc)(size);
}
void *proc_test_calloc(size_t count,size_t size){
    if(fail_alloc)return NULL;
    if(calloc_n<8)callocs[calloc_n]=count*size;
    calloc_n++;
    return (calloc)(count,size);
}
#ifdef KSN_PROC_HOST_FAKE_PIE
void ksn_proc_points_affine_pie(KsnProcPointDst dst,KsnProcPointSrc src,
                                size_t n,const KsnProcAffineQ14 *coeff){
    ksn_proc_points_affine_scalar(dst,src,n,coeff);
}
#endif
ksn_result pocket_kasane_proc_publish(void){return KSN_OK;}
ksn_result pocket_kasane_proc_publish_at(unsigned surface,ksn_rect damage){
    (void)surface;(void)damage;return KSN_OK;
}
void pocket_kasane_invalidate(void){}
JSValue pocket_kasane_proc_resource(JSContext *ctx){
    pocket_proc_image_mode();return JS_NewObject(ctx);
}
JSValue pocket_kasane_proc_resource_at(JSContext *ctx,unsigned surface){
    pocket_proc_image_mode_at(surface);return JS_NewObject(ctx);
}
JSValue pocket_api_throw(JSContext *ctx,const char *code,const char *op,
                         const char *message,bool retryable,const char *outcome){
    (void)retryable;(void)outcome;
    return JS_ThrowTypeError(ctx,"%s %s: %s",code,op,message);
}

static JSContext *ctx;
static JSValue calloc_mark(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)c;(void)self;(void)argc;(void)argv;
    calloc_n=0;return JS_UNDEFINED;
}
static void eval_ok(const char *source){
    JSValue result=JS_Eval(ctx,source,strlen(source),"rom-test.js",JS_EVAL_TYPE_GLOBAL);
    if(JS_IsException(result)){
        JSValue error=JS_GetException(ctx);const char *message=JS_ToCString(ctx,error);
        fprintf(stderr,"QuickJS: %s\nwhile running: %.300s\n",message?message:"exception",source);
        if(message)JS_FreeCString(ctx,message);
        JS_FreeValue(ctx,error);exit(1);
    }
    JS_FreeValue(ctx,result);
}
static int32_t eval_int(const char *source){
    JSValue v=JS_Eval(ctx,source,strlen(source),"rom-test.js",JS_EVAL_TYPE_GLOBAL);
    int32_t n=0;
    REQUIRE(!JS_IsException(v)&&JS_ToInt32(ctx,&n,v)==0);
    JS_FreeValue(ctx,v);return n;
}
static void eval_str(const char *source,char *out,size_t size){
    JSValue v=JS_Eval(ctx,source,strlen(source),"rom-test.js",JS_EVAL_TYPE_GLOBAL);
    REQUIRE(!JS_IsException(v));
    const char *s=JS_ToCString(ctx,v);REQUIRE(s);
    snprintf(out,size,"%s",s);
    JS_FreeCString(ctx,s);JS_FreeValue(ctx,v);
}
static char *slurp(const char *path){
    FILE *f=fopen(path,"rb");REQUIRE(f);
    fseek(f,0,SEEK_END);long n=ftell(f);fseek(f,0,SEEK_SET);
    char *text=(malloc)((size_t)n+1);REQUIRE(text);
    REQUIRE(fread(text,1,(size_t)n,f)==(size_t)n);text[n]=0;fclose(f);
    return text;
}

/* What one draw left behind: the JS outcome, the VM and both frames. */
typedef struct {
    char code[32];
    int status;
    uint32_t steps;
    uint8_t pc,last_pc,depth;
    float reg[KSN_PROC_REGS];
    int16_t pen_x,pen_y;
    bool pen_valid;
    uint16_t scratch_count,scratch_raster,candidate_count,candidate_raster;
} snap;
static ksn_proc_segment seg_a[2][KSN_PROC_SEGMENTS],seg_b[2][KSN_PROC_SEGMENTS];
static void take(snap *s,ksn_proc_segment seg[2][KSN_PROC_SEGMENTS],const char *code){
    memset(s,0,sizeof *s);
    snprintf(s->code,sizeof s->code,"%s",code);
    s->status=vm->status;s->steps=vm->steps;s->pc=vm->pc;s->last_pc=vm->last_pc;
    s->depth=vm->depth;memcpy(s->reg,vm->reg,sizeof s->reg);
    s->pen_x=vm->pen_x;s->pen_y=vm->pen_y;s->pen_valid=vm->pen_valid;
    s->scratch_count=scratch->count;s->scratch_raster=scratch->raster_steps;
    const ksn_proc_frame *c=surfaces[0].candidate;
    s->candidate_count=c->count;s->candidate_raster=c->raster_steps;
    memcpy(seg[0],scratch->segments,scratch->count*sizeof seg[0][0]);
    memcpy(seg[1],c->segments,c->count*sizeof seg[1][0]);
}
static bool same(const snap *a,const snap *b){
    return !memcmp(a,b,sizeof *a)&&
           !memcmp(seg_a[0],seg_b[0],a->scratch_count*sizeof seg_a[0][0])&&
           !memcmp(seg_a[1],seg_b[1],a->candidate_count*sizeof seg_a[1][0]);
}
/* Draw handle h on vector k of IN in a fresh frame, and snapshot it. */
static void draw_one(const char *h,unsigned k,snap *s,ksn_proc_segment seg[2][KSN_PROC_SEGMENTS]){
    char source[96],code[32];
    pocket_proc_end_turn();
    snprintf(source,sizeof source,"run(%s,%u)",h,k);
    eval_str(source,code,sizeof code);
    take(s,seg,code);
}
static const proc_slot *slot_of(const char *h){
    const proc_slot *slot=find_slot((uint32_t)eval_int(h));
    REQUIRE(slot);return slot;
}

static const char prelude[]=
    "globalThis.M=Math;globalThis.PI=Math.PI;globalThis.sin=Math.sin;"
    "globalThis.proc=pocket.kasane.procedural;"
    "globalThis.SILK=[0xffff,0x8c71,0xf800,0x237f,0xffe0,0x07e0,0xfd20,0xf81f];"
    "globalThis.code=f=>{try{const r=f();return r===undefined?'OK':typeof r==='number'?'OK':'VALUE'}"
    "catch(e){const m=String(e);for(const c of ['INVALID_ARGUMENT','LIMIT_EXCEEDED','CLOSED','BUSY',"
    "'OUT_OF_MEMORY'])if(m.includes(c))return c;return m}};"
    "globalThis.expect=(f,c)=>{const got=code(f);"
    "if(got!==c)throw Error('expected '+c+' got '+got+': '+f)};"
    "globalThis.run=(h,k)=>{proc.beginFrame(0);return code(()=>proc.draw(h,IN[k]))};"
    "globalThis.mk=n=>({kind:'affineQ14Points',"
    "x:Array.from({length:n},(_,i)=>i*2-20),y:Array.from({length:n},(_,i)=>60+(i*37)%50-25),"
    "coeff:[8192,0,0,8192,92*16384,88*16384],color:0xef5b});";

static unsigned vectors,drawn,failed,plans_checked;

/* Every captured case: the plan registered from the shipped app's packed
 * text through its decoder (what DERBY registers today), and by name. */
static void equivalence(void){
    const int32_t cases=eval_int("CASES.length");
    for(int32_t i=0;i<cases;i++){
        char source[512];
        snprintf(source,sizeof source,
            "globalThis.C=CASES[%d];globalThis.IN=C.inputs;"
            /* The capture logs no arguments for the runner (load() builds
             * them per horse): the horse's coat and silks, as load() does. */
            "globalThis.A=C.plan==='runner'?[0xdd8c,SILK[%d],SILK[%d]^0x8410]:C.args;"
            "globalThis.ha=proc.register(prog(T[C.plan],A));"
            "calloc_mark();"
            "globalThis.hb=proc.register('derby.'+C.plan,A.slice(0,ROMP[C.plan]))",
            i,i%8,i%8);
        eval_ok(source);
        /* The flash form's only allocation is its header and arguments. */
        const proc_slot *a=slot_of("ha"),*b=slot_of("hb");
        REQUIRE(!a->rom&&b->rom);
        const ksn_proc_sized_plan *pa=a->plan;
        const ksn_proc_rom_plan *pb=b->plan;
        REQUIRE(calloc_n==1&&callocs[0]==ksn_proc_rom_plan_bytes(pb->params));
        /* Same analysis: the same rows were accepted and marked. */
        REQUIRE(pa->valid&&pb->valid&&pa->count==pb->rom->count);
        REQUIRE(pa->fused_count==pb->fused_count&&!memcmp(pa->fused,pb->fused,sizeof pa->fused));
        plans_checked++;
        const int32_t n=eval_int("IN.length");
        for(int32_t k=0;k<n;k++){
            snap sa,sb;
            draw_one("ha",(unsigned)k,&sa,seg_a);
            draw_one("hb",(unsigned)k,&sb,seg_b);
            if(!same(&sa,&sb)){
                char name[32];eval_str("C.plan+' '+C.tag",name,sizeof name);
                fprintf(stderr,"MISMATCH %s vector %d: %s/%s status %d/%d steps %u/%u seg %u/%u\n",
                        name,k,sa.code,sb.code,sa.status,sb.status,(unsigned)sa.steps,
                        (unsigned)sb.steps,sa.scratch_count,sb.scratch_count);
                exit(1);
            }
            vectors++;
            if(!strcmp(sa.code,"OK"))drawn++;else failed++;
        }
        pocket_proc_end_turn();
        eval_ok("proc.unregister(ha);proc.unregister(hb)");
    }
}

/* The fused path's guard: a VM begun from one registration of an entry and
 * run with another registration of the same entry (other arguments) must
 * take the reference loop, and give the reference result. */
static void guard_contract(void){
    const ksn_proc_rom_entry *rom=NULL;
    for(unsigned i=0;i<ksn_proc_rom_plans_count;i++)
        if(!strcmp(ksn_proc_rom_plans[i].name,"derby.runner"))rom=&ksn_proc_rom_plans[i];
    REQUIRE(rom&&rom->params==3);
    const size_t bytes=ksn_proc_rom_plan_bytes(3);
    ksn_proc_rom_plan *a=(calloc)(1,bytes),*b=(calloc)(1,bytes);
    ksn_proc_vm *v=(calloc)(1,sizeof *v),*ref=(calloc)(1,sizeof *ref);
    ksn_proc_frame *f=(calloc)(1,sizeof *f),*g=(calloc)(1,sizeof *g);
    REQUIRE(a&&b&&v&&ref&&f&&g);
    a->params=b->params=3;
    const float colours_a[3]={0xdd8c,0xffff,0x7bef},colours_b[3]={0x1234,0xf800,0x07e0};
    memcpy(a->args,colours_a,sizeof colours_a);memcpy(b->args,colours_b,sizeof colours_b);
    REQUIRE(ksn_proc_rom_plan_prepare(a,rom)&&ksn_proc_rom_plan_prepare(b,rom));
    REQUIRE(a->fused_count>0);
    const float input[KSN_PROC_INPUTS]={120,60,3,1,0.5f,2,0,0};
    /* Same registration: fused, no fallback. */
    fallbacks=0;
    REQUIRE(ksn_proc_rom_plan_begin(v,a,input,f)==KSN_PROC_RUNNING);
    REQUIRE(ksn_proc_rom_plan_run(v,a,false)==KSN_PROC_DONE&&fallbacks==0);
    /* Begun with a, run as b: the guard sees the arguments differ. */
    REQUIRE(ksn_proc_rom_plan_begin(v,a,input,f)==KSN_PROC_RUNNING);
    REQUIRE(ksn_proc_rom_plan_run(v,b,false)==KSN_PROC_DONE&&fallbacks==1);
    REQUIRE(ksn_proc_rom_plan_begin(ref,a,input,g)==KSN_PROC_RUNNING);
    REQUIRE(ksn_proc_rom_plan_run(ref,a,true)==KSN_PROC_DONE);
    REQUIRE(f->count==g->count&&f->raster_steps==g->raster_steps&&
            !memcmp(f->segments,g->segments,f->count*sizeof f->segments[0]));
    /* A VM begun from the array form of the same rows is not this plan. */
    ksn_proc_inst rows[KSN_PROC_CODE];
    memcpy(rows,rom->code,rom->count*sizeof rows[0]);
    const ksn_proc_binding bind={rom->patch,a->args,rom->patches,3};
    REQUIRE(ksn_proc_apply_binding(rows,rom->count,&bind));
    rows[0].b^=1; /* an unused field: the same drawing, other bytes */
    const ksn_proc_program other={rows,rom->count};
    REQUIRE(ksn_proc_begin(v,&other,input,f)==KSN_PROC_RUNNING);
    REQUIRE(ksn_proc_rom_plan_run(v,a,false)==KSN_PROC_DONE&&fallbacks==2);
    /* A NULL binding is refused, not run with the placeholders. */
    const ksn_proc_program flash={rom->code,rom->count};
    REQUIRE(ksn_proc_begin_bound(v,&flash,NULL,input,f)==KSN_PROC_INVALID&&v->status==KSN_PROC_INVALID);
    const unsigned pairs=a->fused_count;
    (free)(a);(free)(b);(free)(v);(free)(ref);(free)(f);(free)(g);
    fallbacks=0;
    printf("guard: a VM from another registration of the same entry, or from other bytes, takes the "
           "reference loop; the same registration takes the fused path (runner: %u fused pairs)\n",pairs);
}

/* Arguments that fill fields, over values each field takes and refuses:
 * the array path and the name path must give the same outcome, and the same
 * frame when both draw. */
static void argument_matrix(void){
    eval_ok("globalThis.VALS=[0,-0,1,2,1.5,-1,64,65,255,256,65535,65536,0.25,-3.5,1e40,-1e40];"
            "globalThis.IN=[[0,0,0,0,0,0,0,0],[10,3,120,-4,5,1,2,80]];"
            "globalThis.base={stands:[3,2,3,3],crowd:[4,3,4,5,5,7,.25,2],runner:[0xdd8c,0xffff,0x7bef]}");
    unsigned outcomes=0,both_ok=0;
    static const char *const plans[]={"stands","crowd","runner"};
    for(unsigned p=0;p<3;p++){
        char source[400];
        snprintf(source,sizeof source,"base.%s.length",plans[p]);
        const int32_t params=eval_int(source);
        for(int32_t at=0;at<params;at++)for(int32_t v=0;v<16;v++){
            char ca[32],cb[32];
            snprintf(source,sizeof source,
                "globalThis.A=base.%s.slice();A[%d]=VALS[%d];globalThis.ha=0;globalThis.hb=0;"
                "code(()=>{ha=proc.register(prog(T.%s,A))})",plans[p],at,v,plans[p]);
            eval_str(source,ca,sizeof ca);
            snprintf(source,sizeof source,"code(()=>{hb=proc.register('derby.%s',A)})",plans[p]);
            eval_str(source,cb,sizeof cb);
            if(strcmp(ca,cb)){
                fprintf(stderr,"ARGUMENT MISMATCH %s $%d=%d: array %s, name %s\n",plans[p],at,v,ca,cb);
                exit(1);
            }
            outcomes++;
            if(!strcmp(ca,"OK")){
                both_ok++;
                for(unsigned k=0;k<2;k++){
                    snap sa,sb;
                    draw_one("ha",k,&sa,seg_a);draw_one("hb",k,&sb,seg_b);
                    REQUIRE(same(&sa,&sb));
                }
                pocket_proc_end_turn();
                eval_ok("proc.unregister(ha);proc.unregister(hb)");
            }
        }
    }
    printf("argument matrix: %u outcomes identical (%u registered and drew the same)\n",outcomes,both_ok);
}

static void errors(void){
    eval_ok(
        "for(const n of ['derby.nope','rail','','derby.','derby.rail ','DERBY.rail','derby.rail\\0'])"
        "expect(()=>proc.register(n),'INVALID_ARGUMENT');"
        /* Arity: exactly the declared arguments; none as undefined/null/[].
         * Too many first: without the check they would be written past the
         * plan's arguments (--mutate). */
        "expect(()=>proc.register('derby.stands',[1,2,3,4,5]),'INVALID_ARGUMENT');"
        "expect(()=>proc.register('derby.crowd',[1,2,3,4,5,6,7,8,9]),'INVALID_ARGUMENT');"
        "expect(()=>proc.register('derby.stands'),'INVALID_ARGUMENT');"
        "expect(()=>proc.register('derby.stands',[1,2,3]),'INVALID_ARGUMENT');"
        "expect(()=>proc.register('derby.rail',[1]),'INVALID_ARGUMENT');"
        "for(const a of [undefined,null,[]])proc.unregister(proc.register('derby.rail',a));"
        "expect(()=>proc.register('derby.stands',5),'INVALID_ARGUMENT');"
        "expect(()=>proc.register('derby.stands',{length:4,0:1,1:1,2:1,3:1}),'INVALID_ARGUMENT');"
        "expect(()=>proc.register('derby.stands','1234'),'INVALID_ARGUMENT');"
        /* Every argument is a finite number, used or not (the array path
         * would ignore an unused one: the name path is stricter). */
        "expect(()=>proc.register('derby.crowd',[4,3,4,'5',5,7,.25,2]),'INVALID_ARGUMENT');"
        "expect(()=>proc.register('derby.crowd',[4,3,4,NaN,5,7,.25,2]),'INVALID_ARGUMENT');"
        "expect(()=>proc.register('derby.crowd',[4,3,4,Infinity,5,7,.25,2]),'INVALID_ARGUMENT');"
        "expect(()=>proc.register('derby.stands',[3,2,3,null]),'INVALID_ARGUMENT');"
        "expect(()=>proc.register('derby.rail',[],mk(8),1),'INVALID_ARGUMENT');"
        /* A getter that calls back in is refused; one that throws fails the
         * registration without a plan left behind (LeakSanitizer). */
        "{const g=[3,2,3,3];Object.defineProperty(g,1,{get(){proc.register('derby.rail');return 2}});"
        "expect(()=>proc.register('derby.stands',g),'INVALID_ARGUMENT')}"
        "{const g=[3,2,3,3];Object.defineProperty(g,2,{get(){throw Error('x')}});"
        "expect(()=>proc.register('derby.stands',g),'INVALID_ARGUMENT')}"
        /* Points on a name, and bad points. */
        "globalThis.hp=proc.register('derby.nil',[],mk(40));"
        "expect(()=>proc.register('derby.nil',[],{kind:'nope'}),'INVALID_ARGUMENT');"
        "expect(()=>proc.register('derby.nil',[],{kind:'affineQ14Points',x:[1],y:[1],coeff:[0,0,0,0,0,0],color:1}),"
        "'INVALID_ARGUMENT');"
        /* Stale handles. */
        "globalThis.hs=proc.register('derby.gate');proc.unregister(hs);"
        "expect(()=>proc.unregister(hs),'CLOSED');"
        "proc.beginFrame(0);expect(()=>proc.draw(hs,[]),'CLOSED')");
    pocket_proc_end_turn();
    /* The typed batch of a named plan against the same batch on the array
     * plan: the typed segments follow the VM's in the candidate. */
    eval_ok("globalThis.ha=proc.register(prog(T.nil),mk(40));globalThis.hb=hp;globalThis.IN=[[]]");
    snap sa,sb;
    draw_one("ha",0,&sa,seg_a);draw_one("hb",0,&sb,seg_b);
    REQUIRE(!strcmp(sa.code,"OK")&&sa.candidate_count==39&&same(&sa,&sb));
    pocket_proc_end_turn();
    eval_ok("proc.unregister(ha);proc.unregister(hb)");
    /* A heap that fails the points after the plan: the plan is freed. */
    fail_malloc=true;
    eval_ok("expect(()=>proc.register('derby.nil',[],mk(40)),'OUT_OF_MEMORY')");
    fail_malloc=false;
    /* No table: every name is unknown. */
    pocket_proc_rom_plans(NULL,5);
    eval_ok("expect(()=>proc.register('derby.rail'),'INVALID_ARGUMENT')");
    pocket_proc_rom_plans(ksn_proc_rom_plans,ksn_proc_rom_plans_count);
    pocket_proc_reset();
}

/* A table the generator would never write: each entry is refused before
 * anything is allocated. bad_long has 65 instructions: without the count
 * check the analysis's copy (64) would overflow (run_pocket_proc_rom_qjs.py
 * --mutate removes that check and expects ASan to stop it). */
static ksn_proc_inst bad_long[65];
static const ksn_proc_inst one[1]={{0,0,0,0,0.0f,0}};
static const ksn_proc_inst rep[3]={{0,0,0,0,0.0f,0},{5,0,0,0,0.0f,0},{6,0,0,0,0.0f,0}};
static const ksn_proc_patch p_far[1]={{3,0,0}},p_param[1]={{1,0,1}},p_field[1]={{1,3,0}},
    p_order[2]={{1,0,0},{0,1,0}};
static const ksn_proc_rom_entry bad_table[]={
    {"bad.empty",one,NULL,0,0,0},
    {"bad.long",bad_long,NULL,65,0,0},
    {"bad.params",one,NULL,1,0,9},
    {"bad.nocode",NULL,NULL,1,0,0},
    {"bad.nopatch",rep,NULL,3,1,1},
    {"bad.far",rep,p_far,3,1,1},
    {"bad.param",rep,p_param,3,1,1},
    {"bad.field",rep,p_field,3,1,1},
    {"bad.order",rep,p_order,3,2,1},
    {"ok.rep",rep,p_param,3,1,2},
};
static void bad_entries(void){
    for(unsigned i=0;i<65;i++)bad_long[i]=(ksn_proc_inst){0,0,0,0,(float)i,0};
    pocket_proc_rom_plans(bad_table,sizeof bad_table/sizeof bad_table[0]);
    calloc_n=0;
    eval_ok("for(const n of ['bad.empty','bad.long','bad.params','bad.nocode','bad.nopatch',"
            "'bad.far','bad.param','bad.field','bad.order']){"
            "expect(()=>proc.register(n),'INVALID_ARGUMENT');"
            "expect(()=>proc.register(n,[1]),'INVALID_ARGUMENT');"
            "expect(()=>proc.register(n,Array(9).fill(1)),'INVALID_ARGUMENT')}");
    REQUIRE(calloc_n==0);
    /* A well-formed REPEAT $1: 0 is refused by the analysis, 3 draws. */
    eval_ok("expect(()=>proc.register('ok.rep',[0,0]),'INVALID_ARGUMENT');"
            "expect(()=>proc.register('ok.rep',[0,1.5]),'INVALID_ARGUMENT');"
            "globalThis.hr=proc.register('ok.rep',[0,3]);proc.beginFrame(0);proc.draw(hr,[]);"
            "proc.unregister(hr)");
    pocket_proc_end_turn();
    pocket_proc_rom_plans(ksn_proc_rom_plans,ksn_proc_rom_plans_count);
    pocket_proc_reset();
}

/* 32 slots: the limit is decided before any allocation, for names as for
 * arrays; a free slot turns the heap's failure into OUT_OF_MEMORY; a churn of
 * both kinds through every slot draws under ASan and frees every plan. */
static void slots_contract(void){
    eval_ok("globalThis.hs=[];for(let i=0;i<32;i++)hs.push(i%2?proc.register('derby.crowd',base.crowd):"
            "proc.register(prog(T.rail)));"
            "globalThis.touched=0;globalThis.trap=[3,2,3,3];"
            "Object.defineProperty(trap,0,{get(){touched++;return 3}});"
            "expect(()=>proc.register('derby.stands',trap),'LIMIT_EXCEEDED');"
            "if(touched)throw Error('full table read the arguments')");
    fail_alloc=true;
    eval_ok("expect(()=>proc.register('derby.stands',[3,2,3,3]),'LIMIT_EXCEEDED')");
    fail_alloc=false;
    eval_ok("proc.unregister(hs[7])");
    fail_alloc=true;
    eval_ok("expect(()=>proc.register('derby.stands',[3,2,3,3]),'OUT_OF_MEMORY')");
    fail_alloc=false;
    eval_ok("hs[7]=proc.register('derby.stands',[3,2,3,3]);"
            "expect(()=>proc.register('derby.gate'),'LIMIT_EXCEEDED');"
            "for(const h of hs)proc.unregister(h)");
    eval_ok("globalThis.NAMES=Object.keys(ROMP);globalThis.sd=4242;"
            "globalThis.rnd=m=>(sd=(sd*1103515245+12345)&0x7fffffff)%m;"
            "globalThis.live=new Array(32).fill(0);"
            "globalThis.argsOf=n=>n==='runner'?base.runner:n==='stands'?base.stands:"
            "n==='crowd'?base.crowd:[];"
            "globalThis.IN=[[3,5,120,-4,5,1,2,80]]");
    for(unsigned round=0;round<12;round++){
        eval_ok("for(let it=0;it<50;it++){const i=rnd(32);"
                "if(live[i]){proc.unregister(live[i]);live[i]=0;continue}"
                "const n=NAMES[rnd(NAMES.length)];"
                "live[i]=rnd(2)?proc.register('derby.'+n,argsOf(n),n==='nil'?mk(2+rnd(127)):undefined):"
                "proc.register(prog(T[n],argsOf(n)),n==='nil'?mk(2+rnd(127)):undefined)}"
                "proc.beginFrame(0);for(const h of live)if(h)code(()=>proc.draw(h,IN[0]))");
        pocket_proc_end_turn();
    }
    pocket_proc_reset();
}

int main(int argc,char **argv){
    REQUIRE(argc==3);
    pocket_proc_rom_plans(ksn_proc_rom_plans,ksn_proc_rom_plans_count);
    JSRuntime *rt=JS_NewRuntime();REQUIRE(rt);
    ctx=JS_NewContext(rt);REQUIRE(ctx);
    JSValue global=JS_GetGlobalObject(ctx),pocket=JS_NewObject(ctx),kasane=JS_NewObject(ctx);
    REQUIRE(pocket_proc_install(ctx,kasane)==ESP_OK);
    REQUIRE(JS_SetPropertyStr(ctx,pocket,"kasane",kasane)>=0);
    REQUIRE(JS_SetPropertyStr(ctx,global,"pocket",pocket)>=0);
    JS_FreeValue(ctx,global);
    eval_ok(prelude);
    char *app=slurp(argv[1]);eval_ok(app);(free)(app);
    /* The table's declared argument counts, by plan. */
    eval_ok("globalThis.ROMP={}");
    for(unsigned i=0;i<ksn_proc_rom_plans_count;i++){
        char source[96];
        REQUIRE(!strncmp(ksn_proc_rom_plans[i].name,"derby.",6));
        snprintf(source,sizeof source,"ROMP['%s']=%u",ksn_proc_rom_plans[i].name+6,
                 (unsigned)ksn_proc_rom_plans[i].params);
        eval_ok(source);
    }
    /* calloc_mark() resets the allocation log between two registrations. */
    JSValue g=JS_GetGlobalObject(ctx);
    REQUIRE(JS_SetPropertyStr(ctx,g,"calloc_mark",
                              JS_NewCFunction(ctx,calloc_mark,"calloc_mark",0))>=0);
    JS_FreeValue(ctx,g);
    char *cases=slurp(argv[2]);
    {
        size_t n=strlen(cases);
        char *source=(malloc)(n+32);REQUIRE(source);
        snprintf(source,n+32,"globalThis.CASES=%s",cases);
        eval_ok(source);(free)(source);(free)(cases);
    }
    fallbacks=0;
    equivalence();
    REQUIRE(fallbacks==0);
    guard_contract();
    printf("equivalence: %u registrations, %u vectors (%u drawn, %u failed the same way) identical: "
           "status, steps, pc, registers, pen, every segment and raster steps\n",
           plans_checked,vectors,drawn,failed);
    pocket_proc_end_turn();
    argument_matrix();
    errors();
    bad_entries();
    slots_contract();
    pocket_proc_reset();
    JS_FreeContext(ctx);JS_FreeRuntime(rt);
    printf("PASS built-in plans by name: same output as arrays, arity/name/value errors, bad table "
           "entries before allocation, limit before allocation, OOM, churn\n");
    return 0;
}
