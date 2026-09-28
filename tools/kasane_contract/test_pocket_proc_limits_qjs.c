/* Real QuickJS contract for the widened procedural limits and unregister().
 * Built under ASan/UBSan by run_pocket_proc_limits_qjs.py: LeakSanitizer is
 * what proves that a register/unregister cycle frees both allocations, and
 * that pocket_proc_reset() after unregister() frees nothing twice. */
#include "quickjs.h"
#include "pocket_proc.h"
#include "pocket_api.h"
#include "ksn_proc_plan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(x) do { if(!(x)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);} } while(0)
#define POINTS 128u
static uint16_t actual[KSN_PROC_W*KSN_PROC_H],expected[KSN_PROC_W*KSN_PROC_H];
static uint16_t other[KSN_PROC_W*KSN_PROC_H];
static ksn_proc_frame reference_frame;
static ksn_rect last_damage;
static unsigned damage_publishes;

#ifdef KSN_PROC_HOST_FAKE_PIE
void ksn_proc_points_affine_pie(KsnProcPointDst dst,KsnProcPointSrc src,
                                size_t n,const KsnProcAffineQ14 *coeff){
    ksn_proc_points_affine_scalar(dst,src,n,coeff);
}
#endif
ksn_result pocket_kasane_proc_publish(void){return KSN_OK;}
ksn_result pocket_kasane_proc_publish_at(unsigned surface,ksn_rect damage){
    (void)surface;last_damage=damage;damage_publishes++;return KSN_OK;
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
static void eval_ok(JSContext *ctx,const char *source){
    JSValue result=JS_Eval(ctx,source,strlen(source),"limits-test.js",JS_EVAL_TYPE_GLOBAL);
    if(JS_IsException(result)){
        JSValue error=JS_GetException(ctx);const char *message=JS_ToCString(ctx,error);
        fprintf(stderr,"QuickJS: %s\nwhile running: %s\n",message?message:"exception",source);
        if(message)JS_FreeCString(ctx,message);
        JS_FreeValue(ctx,error);exit(1);
    }
    JS_FreeValue(ctx,result);
}
/* Commit a pending frame and read it back through the backdrop path. */
static void settle(uint16_t *pixels){
    REQUIRE(pocket_proc_pending());
    REQUIRE(pocket_proc_backdrop(NULL,0,KSN_PROC_H,pixels)==KSN_OK);
    pocket_proc_present_result(KSN_OK);
    REQUIRE(!pocket_proc_pending());
}
static void reference_polyline(uint16_t background,const int16_t *x,const int16_t *y,
                               unsigned n,uint16_t color){
    for(unsigned i=0;i<KSN_PROC_W*KSN_PROC_H;i++)expected[i]=background;
    memset(&reference_frame,0,sizeof reference_frame);
    reference_frame.ready=true;reference_frame.count=(uint16_t)(n-1);
    for(unsigned i=1;i<n;i++)
        reference_frame.segments[i-1]=(ksn_proc_segment){x[i-1],y[i-1],x[i],y[i],color};
    REQUIRE(ksn_proc_render_band(&reference_frame,expected,0,KSN_PROC_H));
}
static unsigned count_color(const uint16_t *pixels,uint16_t color){
    unsigned n=0;
    for(unsigned i=0;i<KSN_PROC_W*KSN_PROC_H;i++)n+=pixels[i]==color;
    return n;
}

static const char prelude[]=
    "globalThis.proc=pocket.kasane.procedural;"
    "globalThis.code=f=>{try{const r=f();return r===undefined?'OK':'VALUE'}catch(e){"
    "const m=String(e);for(const c of ['INVALID_ARGUMENT','LIMIT_EXCEEDED','CLOSED','BUSY'])"
    "if(m.includes(c))return c;return m}};"
    "globalThis.expect=(f,c)=>{const got=code(f);"
    "if(got!==c)throw Error('expected '+c+' got '+got+': '+f)};"
    "globalThis.dot=[[0,0,0,0,5,0],[0,1,0,0,5,0],[8,0,0,1,0,65535]];"
    /* Same 128-point trace as points_128() below. */
    "globalThis.trace={kind:'affineQ14Points',"
    "x:Array.from({length:128},(_,i)=>i*2-20),"
    "y:Array.from({length:128},(_,i)=>60+(i*37)%50-25),"
    "coeff:[16000,1600,-1000,16384,5*16384,-3*16384],color:0x07e0};";

static void unregister_contract(JSContext *ctx){
    eval_ok(ctx,
        "let h=proc.register(dot);"
        "if(proc.unregister.length!==1)throw Error('unregister arity');"
        "proc.beginFrame(0);proc.draw(h,[]);proc.commit()");
    settle(actual);
    REQUIRE(actual[5*KSN_PROC_W+5]==0xffff&&count_color(actual,0xffff)==1);
    eval_ok(ctx,
        "expect(()=>proc.unregister(h),'OK');"
        "proc.beginFrame(0);"
        "expect(()=>proc.draw(h,[]),'CLOSED');"
        "expect(()=>proc.unregister(h),'CLOSED');"
        "expect(()=>proc.unregister(987654),'CLOSED');"
        /* Handle 0 is never issued; an empty slot also stores 0. */
        "expect(()=>proc.unregister(0),'CLOSED');"
        "proc.beginFrame(0);expect(()=>proc.draw(0,[]),'CLOSED');"
        "expect(()=>proc.unregister(),'INVALID_ARGUMENT');"
        "expect(()=>proc.unregister(h,1),'INVALID_ARGUMENT');"
        "expect(()=>proc.unregister('1'),'INVALID_ARGUMENT');"
        "expect(()=>proc.unregister(-1),'INVALID_ARGUMENT');"
        "expect(()=>proc.unregister(1.5),'INVALID_ARGUMENT');");
    pocket_proc_end_turn();

    /* Fill every slot, reject the 33rd, free one in the middle, and prove
     * that the freed slot is reused under a new, never-issued handle. */
    pocket_proc_reset();
    eval_ok(ctx,
        "let hs=[];for(let i=0;i<32;i++){hs.push(proc.register("
        "[[0,0,0,0,i,0],[0,1,0,0,i,0],[8,0,0,1,0,i+1]]));"
        "if(i&&hs[i]<=hs[i-1])throw Error('handles must increase')}"
        "expect(()=>proc.register(dot),'LIMIT_EXCEEDED');"
        "expect(()=>proc.register(dot,trace),'LIMIT_EXCEEDED');"
        "const gone=hs[13];proc.unregister(gone);"
        "const again=proc.register(dot);"
        "if(hs.includes(again)||again<=hs[31])throw Error('handle reused');"
        "expect(()=>proc.register(dot),'LIMIT_EXCEEDED');"
        "proc.beginFrame(0);"
        "expect(()=>proc.draw(gone,[]),'CLOSED');"
        "proc.beginFrame(0);"
        "for(const k of hs)if(k!==gone)proc.draw(k,[]);"
        "proc.draw(again,[]);proc.commit()");
    settle(actual);
    for(unsigned i=0;i<32;i++)if(i!=5)
        REQUIRE(actual[i*KSN_PROC_W+i]==(i==13?0:(uint16_t)(i+1)));
    REQUIRE(actual[5*KSN_PROC_W+5]==0xffff); /* dot overdraws plan 5 */

    /* A frame already holds its segments: unregistering mid-frame changes
     * nothing that commit() or presentation reads. */
    eval_ok(ctx,
        "proc.beginFrame(0);proc.draw(again,[]);"
        "expect(()=>proc.unregister(again),'OK');"
        "expect(()=>proc.draw(again,[]),'CLOSED');");
    /* The failed draw above ended the frame; rebuild with the surviving plan. */
    eval_ok(ctx,
        "proc.beginFrame(0);proc.draw(hs[20],[]);"
        "proc.unregister(hs[20]);proc.commit()");
    settle(actual);
    REQUIRE(actual[20*KSN_PROC_W+20]==21&&count_color(actual,21)==1);

    /* unregister is under the same reentrancy guard as every other call. */
    eval_ok(ctx,
        "let busy=false;const inputs=[];"
        "Object.defineProperty(inputs,0,{get(){busy=code(()=>proc.unregister(hs[1]))==='BUSY';return 0}});"
        "proc.beginFrame(0);proc.draw(hs[1],inputs);proc.commit();"
        "if(!busy)throw Error('reentrant unregister accepted')");
    settle(actual);
    REQUIRE(actual[1*KSN_PROC_W+1]==2);

    /* Register/unregister churn with the largest point batch. ASan's leak
     * check at exit fails the test if either allocation is dropped. */
    eval_ok(ctx,
        "let last=0;for(let i=0;i<600;i++){const k=proc.register(dot,trace);"
        "if(k<=last)throw Error('handle order');last=k;proc.unregister(k)}");
    /* reset() after unregister() must not free a released slot again. */
    eval_ok(ctx,"proc.unregister(hs[2]);proc.register(dot,trace)");
    pocket_proc_reset();
    eval_ok(ctx,"expect(()=>proc.unregister(hs[3]),'CLOSED')");
}

static void inputs_contract(JSContext *ctx){
    /* Eight INPUT/SET/PLOT groups through r8 and r15: input k -> x, row
     * 10+15k -> y. Missing inputs read as zero. */
    eval_ok(ctx,
        "const wideCode=[];for(let k=0;k<8;k++)wideCode.push([1,8,k,0,0,0],"
        "[0,15,0,0,10+15*k,0],[8,0,8,15,0,65535]);"
        "globalThis.wide=proc.register(wideCode);"
        "expect(()=>proc.register([[1,0,8,0,0,0]]),'INVALID_ARGUMENT');"
        "expect(()=>proc.register([[0,16,0,0,0,0]]),'INVALID_ARGUMENT');"
        "expect(()=>proc.register([[2,15,14,16,0,0]]),'INVALID_ARGUMENT');"
        "proc.register([[1,15,7,0,0,0],[2,14,15,15,0,0]])");
    static const unsigned lengths[]={0,4,8};
    for(unsigned t=0;t<3;t++){
        char source[256];
        snprintf(source,sizeof source,
                 "proc.beginFrame(0);proc.draw(wide,"
                 "Array.from({length:%u},(_,k)=>30+20*k));proc.commit()",lengths[t]);
        eval_ok(ctx,source);
        settle(actual);
        REQUIRE(count_color(actual,0xffff)==8);
        for(unsigned k=0;k<8;k++){
            unsigned x=k<lengths[t]?30+20*k:0,y=10+15*k;
            REQUIRE(actual[y*KSN_PROC_W+x]==0xffff);
        }
    }
    /* A four-input call is the same frame as its explicit zero padding. */
    eval_ok(ctx,"proc.beginFrame(0);proc.draw(wide,[31,52,73,94]);proc.commit()");
    settle(actual);
    eval_ok(ctx,"proc.beginFrame(0);proc.draw(wide,[31,52,73,94,0,0,0,0]);proc.commit()");
    settle(other);
    REQUIRE(memcmp(actual,other,sizeof actual)==0);
    eval_ok(ctx,
        "proc.beginFrame(0);"
        "expect(()=>proc.draw(wide,[0,0,0,0,0,0,0,0,0]),'INVALID_ARGUMENT');"
        "proc.beginFrame(0);"
        "expect(()=>proc.draw(wide,[0,0,0,0,0,0,0,NaN]),'INVALID_ARGUMENT');"
        "proc.beginFrame(0);"
        "expect(()=>proc.draw(wide,{length:0}),'INVALID_ARGUMENT');"
        "proc.beginFrame(0);"
        "expect(()=>proc.draw(wide),'INVALID_ARGUMENT')");
    pocket_proc_end_turn();
}

static void depth_contract(JSContext *ctx){
    eval_ok(ctx,
        "const nest=n=>{const c=[];for(let i=0;i<n;i++)c.push([5,0,1,0,0,0]);"
        "c.push(...dot);for(let i=0;i<n;i++)c.push([6,0,0,0,0,0]);return c};"
        "const deep=proc.register(nest(8));"
        "expect(()=>proc.register(nest(9)),'INVALID_ARGUMENT');"
        "proc.beginFrame(0);proc.draw(deep,[]);proc.commit()");
    settle(actual);
    REQUIRE(actual[5*KSN_PROC_W+5]==0xffff);
}

static void points_128(JSContext *ctx,uint32_t *pie_expected){
    _Alignas(16) int16_t x[POINTS],y[POINTS],out_x[POINTS],out_y[POINTS];
    for(unsigned i=0;i<POINTS;i++){
        x[i]=(int16_t)(i*2-20);
        y[i]=(int16_t)(60+(i*37)%50-25);
    }
    const KsnProcAffineQ14 coeff={16000,1600,-1000,16384,5*16384,-3*16384};
    ksn_proc_points_affine_scalar((KsnProcPointDst){out_x,out_y},
                                  (KsnProcPointSrc){x,y},POINTS,&coeff);
    eval_ok(ctx,
        "globalThis.long=proc.register([[0,0,0,0,0,0]],trace);"
        "const t129={...trace,x:[...trace.x,0],y:[...trace.y,0]};"
        "expect(()=>proc.register([[0,0,0,0,0,0]],t129),'INVALID_ARGUMENT');"
        "proc.beginFrame(3);proc.draw(long,[]);proc.commit()");
    settle(actual);
    reference_polyline(3,out_x,out_y,POINTS,0x07e0);
    REQUIRE(memcmp(actual,expected,sizeof actual)==0);
    ++*pie_expected;
}

/* Batches here have 2 or 4 points: below the eight-point PIE block, so even
 * the fake-PIE build must count them as scalar. */
static void off_panel(JSContext *ctx,uint32_t *scalar_expected){
    /* Typed points now share the VM coordinate range: -480..720 inclusive. */
    static const int16_t edge_x[]={-480,720},edge_y[]={-480,720};
    eval_ok(ctx,
        "const batch=(x,y)=>({kind:'affineQ14Points',x,y,"
        "coeff:[16384,0,0,16384,0,0],color:0xf800});"
        "globalThis.edge=proc.register([[0,0,0,0,0,0]],batch([-480,720],[-480,720]));"
        "const outside=[[[-481,0],[0,0]],[[0,721],[0,0]],[[0,0],[-481,0]],[[0,0],[0,721]]];"
        "for(const [x,y] of outside){const k=proc.register([[0,0,0,0,0,0]],batch(x,y));"
        "proc.beginFrame(0);let message='';"
        "try{proc.draw(k,[])}catch(e){message=String(e)}"
        "if(!message.includes('INVALID_ARGUMENT')||!message.includes('-480..720'))"
        "throw Error('off-range point accepted: '+message);proc.unregister(k)}"
        "globalThis.away=proc.register([[0,0,0,0,0,0]],batch([-100,-50,300,700],[10,-200,-150,-300]));"
        "proc.beginFrame(0);proc.draw(edge,[]);proc.commit()");
    settle(actual);
    reference_polyline(0,edge_x,edge_y,2,0xf800);
    REQUIRE(memcmp(actual,expected,sizeof actual)==0);
    REQUIRE(count_color(actual,0xf800)==KSN_PROC_H); /* the visible diagonal */
    eval_ok(ctx,"proc.beginFrame(0);proc.draw(away,[]);proc.commit()");
    settle(actual);
    REQUIRE(count_color(actual,0)==KSN_PROC_W*KSN_PROC_H);
    *scalar_expected+=2;

    /* Image mode: damage is clamped to the panel and every source row reads
     * back through the cached eight-row band renderer. */
    eval_ok(ctx,"proc.resource();proc.beginFrame(0);proc.draw(away,[]);proc.commit()");
    REQUIRE(damage_publishes>0);
    pocket_proc_present_result(KSN_OK);
    unsigned before=damage_publishes;
    eval_ok(ctx,"proc.beginFrame(0);proc.draw(edge,[]);proc.draw(away,[]);proc.commit()");
    REQUIRE(damage_publishes==before+1);
    REQUIRE(last_damage.x0>=0&&last_damage.y0>=0&&last_damage.x0<=last_damage.x1&&
            last_damage.y0<=last_damage.y1&&last_damage.x1<=KSN_PROC_W&&
            last_damage.y1<=KSN_PROC_H);
    REQUIRE(last_damage.x0==0&&last_damage.y0==0&&
            last_damage.x1==KSN_PROC_W&&last_damage.y1==KSN_PROC_H);
    ksn_image_port image;pocket_proc_image_port(&image);
    uint16_t span[KSN_PROC_W];uint8_t alpha[KSN_PROC_W];
    for(unsigned y=KSN_PROC_H;y-->0;){
        REQUIRE(image.read_span(image.ctx,0,0,(uint16_t)y,0,KSN_PROC_W,span,alpha)==KSN_OK);
        REQUIRE(memcmp(span,expected+y*KSN_PROC_W,sizeof span)==0);
    }
    pocket_proc_present_result(KSN_OK);
    *scalar_expected+=3;
}

int main(void){
    JSRuntime *rt=JS_NewRuntime();REQUIRE(rt);
    JSContext *ctx=JS_NewContext(rt);REQUIRE(ctx);
    JSValue global=JS_GetGlobalObject(ctx),pocket=JS_NewObject(ctx),kasane=JS_NewObject(ctx);
    REQUIRE(pocket_proc_install(ctx,kasane)==ESP_OK);
    REQUIRE(JS_SetPropertyStr(ctx,pocket,"kasane",kasane)>=0);
    REQUIRE(JS_SetPropertyStr(ctx,global,"pocket",pocket)>=0);
    JS_FreeValue(ctx,global);
    eval_ok(ctx,prelude);
    unregister_contract(ctx);
    inputs_contract(ctx);
    depth_contract(ctx);
    pocket_proc_reset();
    uint32_t big=0,small=0,typed;
    points_128(ctx,&big);
    off_panel(ctx,&small);
    typed=big+small;
    uint32_t scalar=0,pie=0;
    pocket_proc_batch_counts(&scalar,&pie);
#ifdef KSN_PROC_HOST_FAKE_PIE
    REQUIRE(scalar==small&&pie==big);
#else
    REQUIRE(scalar==typed&&pie==0);
#endif
    pocket_proc_reset();
    JS_FreeContext(ctx);JS_FreeRuntime(rt);
    printf("PASS procedural limits: 32 plans + unregister (CLOSED, no handle reuse, "
           "slot reuse, reentrancy, 600-cycle churn), 0/4/8 inputs, r8..r15, "
           "8-deep loops, 128 points (%u typed batches), -480..720 typed points\n",
           (unsigned)typed);
    return 0;
}
