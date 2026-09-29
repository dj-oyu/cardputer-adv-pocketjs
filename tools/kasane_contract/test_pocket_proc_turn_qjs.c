/* Real QuickJS (with the L2c VM) contract for the procedural frame across a
 * turn boundary, the plan limit's precedence over allocation, and per-count
 * point allocation. Built by run_pocket_proc_limits_qjs.py under ASan/UBSan
 * with proc_alloc_hook.h force-included into these sources only, so
 * pocket_proc.c's allocations can be failed and measured while the prebuilt
 * engine allocates normally.
 *
 * The park is the VM's own: frame() is entered through JS_VMCall, asks for a
 * yield from a native, and parks at the next backward branch -- the same
 * suspension the 8 ms turn budget causes on the device. What app_session.c
 * does at the end of that host turn is modelled by calling (or not calling)
 * pocket_proc_end_turn(); the choice itself is tools/test_session_dispatch.c. */
#include "quickjs.h"
#include "quickjs-vm.h"
#include "pocket_proc.h"
#include "pocket_api.h"
#include "ksn_proc_plan.h"
#include "ksn_proc_points_pie.h"
#include <stdio.h>
#include <string.h>

#define REQUIRE(x) do { if(!(x)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);} } while(0)
static uint16_t actual[KSN_PROC_W*KSN_PROC_H],expected[KSN_PROC_W*KSN_PROC_H];
static ksn_proc_frame reference_frame;

static bool fail_alloc;
static size_t last_malloc;
void *proc_test_malloc(size_t size){
    if(fail_alloc)return NULL;
    last_malloc=size;return (malloc)(size);
}
void *proc_test_calloc(size_t count,size_t size){
    if(fail_alloc)return NULL;
    return (calloc)(count,size);
}

#if KSN_PROC_POINTS_HAS_PIE
/* The device kernel's memory traversal without its arithmetic: 128-bit
 * aligned loads and stores for every whole block of eight, the scalar
 * reference for the tail. ASan fails the run if a block reaches past the
 * per-count allocation; the alignment check fails it if a plane is not on 16. */
static unsigned blocks_run;
void ksn_proc_points_affine_pie(KsnProcPointDst dst,KsnProcPointSrc src,
                                size_t n,const KsnProcAffineQ14 *coeff){
    const size_t blocks=n/8;
    for(size_t b=0;b<blocks;b++){
        const size_t i=b*8;
        REQUIRE((((uintptr_t)(src.x+i))|((uintptr_t)(src.y+i))|
                 ((uintptr_t)(dst.x+i))|((uintptr_t)(dst.y+i)))%16==0);
        int16_t sx[8],sy[8],dx[8],dy[8];
        memcpy(sx,src.x+i,16);memcpy(sy,src.y+i,16);
        ksn_proc_points_affine_scalar((KsnProcPointDst){dx,dy},
                                      (KsnProcPointSrc){sx,sy},8,coeff);
        memcpy(dst.x+i,dx,16);memcpy(dst.y+i,dy,16);
        blocks_run++;
    }
    if(n>blocks*8)
        ksn_proc_points_affine_scalar((KsnProcPointDst){dst.x+blocks*8,dst.y+blocks*8},
                                      (KsnProcPointSrc){src.x+blocks*8,src.y+blocks*8},
                                      n-blocks*8,coeff);
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
static JSValue request_yield(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argc;(void)argv;
    JS_VMRequestYield(JS_GetRuntime(ctx));
    return JS_UNDEFINED;
}
static void eval_ok(JSContext *ctx,const char *source){
    JSValue result=JS_Eval(ctx,source,strlen(source),"turn-test.js",JS_EVAL_TYPE_GLOBAL);
    if(JS_IsException(result)){
        JSValue error=JS_GetException(ctx);const char *message=JS_ToCString(ctx,error);
        fprintf(stderr,"QuickJS: %s\nwhile running: %s\n",message?message:"exception",source);
        if(message)JS_FreeCString(ctx,message);
        JS_FreeValue(ctx,error);exit(1);
    }
    JS_FreeValue(ctx,result);
}
/* frame() through the host's own entry. Returns true if it parked. */
static bool call_frame(JSContext *ctx,bool *threw){
    JSValue global=JS_GetGlobalObject(ctx);
    JSValue frame=JS_GetPropertyStr(ctx,global,"frame");
    JSValue v=JS_VMCall(ctx,frame,JS_UNDEFINED,0,NULL);
    JS_FreeValue(ctx,frame);JS_FreeValue(ctx,global);
    bool parked=JS_VMSuspended(JS_GetRuntime(ctx));
    *threw=!parked&&JS_IsException(v);
    if(*threw)JS_FreeValue(ctx,JS_GetException(ctx));
    JS_FreeValue(ctx,v);
    return parked;
}
static bool resume(JSContext *ctx,bool *threw){
    JSValue v=JS_VMResume(ctx);
    bool parked=JS_VMSuspended(JS_GetRuntime(ctx));
    *threw=!parked&&JS_IsException(v);
    if(*threw)JS_FreeValue(ctx,JS_GetException(ctx));
    JS_FreeValue(ctx,v);
    return parked;
}
static void settle(uint16_t *pixels){
    REQUIRE(pocket_proc_pending());
    REQUIRE(pocket_proc_backdrop(NULL,0,KSN_PROC_H,pixels)==KSN_OK);
    pocket_proc_present_result(KSN_OK);
    REQUIRE(!pocket_proc_pending());
}
static unsigned count_color(const uint16_t *pixels,uint16_t color){
    unsigned n=0;
    for(unsigned i=0;i<KSN_PROC_W*KSN_PROC_H;i++)n+=pixels[i]==color;
    return n;
}

static const char prelude[]=
    "globalThis.proc=pocket.kasane.procedural;"
    "globalThis.code=f=>{try{const r=f();return r===undefined?'OK':'VALUE'}catch(e){"
    "const m=String(e);for(const c of ['INVALID_ARGUMENT','LIMIT_EXCEEDED','CLOSED','BUSY',"
    "'OUT_OF_MEMORY'])if(m.includes(c))return c;return m}};"
    "globalThis.expect=(f,c)=>{const got=code(f);"
    "if(got!==c)throw Error('expected '+c+' got '+got+': '+f)};"
    "globalThis.dotAt=(x,y,c)=>[[0,0,0,0,x,0],[0,1,0,0,y,0],[8,0,0,1,0,c]];"
    "globalThis.mk=n=>({kind:'affineQ14Points',"
    "x:Array.from({length:n},(_,i)=>i*2-20),"
    "y:Array.from({length:n},(_,i)=>60+(i*37)%50-25),"
    "coeff:[16384,0,0,16384,0,0],color:0x07e0});"
    "globalThis.spin=()=>{for(let i=0;i<2;i++){}};";

/* Bug 1: a frame the VM parks between beginFrame() and commit(). */
static void park_contract(JSContext *ctx){
    bool threw;
    eval_ok(ctx,
        "globalThis.a=proc.register(dotAt(10,10,0xffff));"
        "globalThis.b=proc.register(dotAt(20,20,0xf800));"
        "globalThis.c=proc.register(dotAt(30,30,0x001f));"
        "globalThis.frame=()=>{proc.beginFrame(0);requestYield();spin();"
        "proc.draw(a,[]);proc.commit()}");
    /* What the firmware did: the parked host turn ends the procedural frame,
     * and the resumed frame() meets BUSY (procedural-limits-device.md sec.5). */
    REQUIRE(call_frame(ctx,&threw));
    pocket_proc_end_turn();
    REQUIRE(!resume(ctx,&threw)&&threw);
    REQUIRE(!pocket_proc_pending());
    pocket_proc_end_turn();

    /* The fix: a parked turn leaves the frame open; the continuation draws
     * and commits the same frame. */
    REQUIRE(call_frame(ctx,&threw));
    REQUIRE(!resume(ctx,&threw)&&!threw);
    pocket_proc_end_turn();
    settle(actual);
    REQUIRE(actual[10*KSN_PROC_W+10]==0xffff&&count_color(actual,0xffff)==1);

    /* Parked three times inside one frame, one draw per host turn. */
    eval_ok(ctx,
        "globalThis.frame=()=>{proc.beginFrame(0);"
        "for(const h of [a,b,c]){requestYield();spin();proc.draw(h,[])}"
        "proc.commit()}");
    unsigned parks=0;
    bool parked=call_frame(ctx,&threw);
    while(parked){parks++;parked=resume(ctx,&threw);}
    REQUIRE(parks==3&&!threw);
    pocket_proc_end_turn();
    settle(actual);
    REQUIRE(actual[10*KSN_PROC_W+10]==0xffff&&actual[20*KSN_PROC_W+20]==0xf800&&
            actual[30*KSN_PROC_W+30]==0x001f&&count_color(actual,0)==KSN_PROC_W*KSN_PROC_H-3);

    /* A parked frame that is terminated instead of resumed (stop, Back's
     * teardown, runaway): app_vm_prepare_stop() ends the turn, and whatever
     * JS runs next (the stop hook) finds no open frame to add to or commit. */
    eval_ok(ctx,
        "globalThis.frame=()=>{proc.beginFrame(0);proc.draw(b,[]);requestYield();spin();"
        "proc.draw(a,[]);proc.commit()}");
    REQUIRE(call_frame(ctx,&threw));
    JS_VMTerminate(JS_GetRuntime(ctx));
    REQUIRE(!resume(ctx,&threw));
    pocket_proc_end_turn();
    REQUIRE(!pocket_proc_pending());
    eval_ok(ctx,"expect(()=>proc.draw(c,[]),'BUSY');expect(()=>proc.commit(),'BUSY')");

    /* A frame that throws with its frame open: nothing is carried either. */
    eval_ok(ctx,"globalThis.frame=()=>{proc.beginFrame(0);proc.draw(b,[]);throw Error('x')}");
    REQUIRE(!call_frame(ctx,&threw)&&threw);
    pocket_proc_end_turn();
    eval_ok(ctx,"expect(()=>proc.draw(c,[]),'BUSY');expect(()=>proc.commit(),'BUSY')");
    REQUIRE(!pocket_proc_pending());
    pocket_proc_reset();
}

/* Back's save turn (app_tick(0x2000)): yield is off, so the continuation runs
 * the parked frame() to its commit() and leaves that frame pending; the save
 * frame(0x2000) that follows in the same host turn opens a frame first, as
 * MEGADEMO's does. Without a present between the two its beginFrame() is BUSY
 * (the save is lost and the session ends EXECUTION FAILED); app_tick() now
 * presents there, which is settle() here. */
static void leave_contract(JSContext *ctx){
    bool threw;
    eval_ok(ctx,
        "globalThis.a=proc.register(dotAt(10,10,0xffff));"
        "globalThis.saving=false;"
        "globalThis.frame=()=>{proc.beginFrame(0);if(!saving){requestYield();spin()}"
        "proc.draw(a,[]);proc.commit()}");
    /* Before: continuation, then the save frame at once. */
    REQUIRE(call_frame(ctx,&threw));
    REQUIRE(!resume(ctx,&threw)&&!threw);
    pocket_proc_end_turn();
    REQUIRE(pocket_proc_pending());
    eval_ok(ctx,"saving=true");
    REQUIRE(!call_frame(ctx,&threw)&&threw);
    eval_ok(ctx,"expect(()=>proc.beginFrame(0),'BUSY')");
    pocket_proc_end_turn();
    settle(actual);
    /* After: the finished frame is presented first; the save frame opens,
     * draws and commits. */
    eval_ok(ctx,"saving=false");
    REQUIRE(call_frame(ctx,&threw));
    REQUIRE(!resume(ctx,&threw)&&!threw);
    pocket_proc_end_turn();
    settle(actual);
    REQUIRE(actual[10*KSN_PROC_W+10]==0xffff);
    eval_ok(ctx,"saving=true");
    REQUIRE(!call_frame(ctx,&threw)&&!threw);
    pocket_proc_end_turn();
    settle(actual);
    REQUIRE(actual[10*KSN_PROC_W+10]==0xffff&&count_color(actual,0xffff)==1);
    pocket_proc_reset();
}

/* Bug 2: the 33rd register() is LIMIT_EXCEEDED before any array is read or
 * any byte allocated, so a low heap cannot turn it into OUT_OF_MEMORY. */
static void limit_contract(JSContext *ctx){
    eval_ok(ctx,
        "globalThis.hs=[];for(let i=0;i<32;i++)hs.push(proc.register(dotAt(i,i,i+1)));"
        "globalThis.touched=0;"
        "globalThis.trap=[0];Object.defineProperty(trap,0,"
        "{get(){touched++;return [0,0,0,0,0,0]}});"
        "globalThis.ptrap={get kind(){touched++;return 'affineQ14Points'},x:[0,1],y:[0,1],"
        "coeff:[16384,0,0,16384,0,0],color:1};"
        "expect(()=>proc.register(trap),'LIMIT_EXCEEDED');"
        "expect(()=>proc.register(dotAt(0,0,1),ptrap),'LIMIT_EXCEEDED');"
        "if(touched)throw Error('full table read the arguments: '+touched)");
    fail_alloc=true;
    eval_ok(ctx,
        "expect(()=>proc.register(dotAt(0,0,1)),'LIMIT_EXCEEDED');"
        "expect(()=>proc.register(dotAt(0,0,1),mk(128)),'LIMIT_EXCEEDED')");
    fail_alloc=false;
    /* With a slot free, the same failure is the heap's and says so. */
    eval_ok(ctx,"proc.unregister(hs[7])");
    fail_alloc=true;
    eval_ok(ctx,
        "expect(()=>proc.register(dotAt(0,0,1)),'OUT_OF_MEMORY');"
        "expect(()=>proc.register(dotAt(0,0,1),mk(40)),'OUT_OF_MEMORY')");
    fail_alloc=false;
    eval_ok(ctx,
        "hs[7]=proc.register(dotAt(0,0,1),mk(40));"
        "expect(()=>proc.register(dotAt(0,0,1)),'LIMIT_EXCEEDED')");
    pocket_proc_reset();
}

/* Improvement 3: a batch allocates for its own count, rounded to whole
 * eight-lane planes, and draws the same pixels as the scalar reference. */
static void sizing_contract(JSContext *ctx,unsigned *pie_expected){
    static const unsigned counts[]={2,7,8,9,40,63,64,65,120,127,128};
    for(unsigned k=0;k<sizeof counts/sizeof counts[0];k++){
        const unsigned n=counts[k];
        char source[160];
        snprintf(source,sizeof source,
                 "globalThis.pb=proc.register([[0,0,0,0,0,0]],mk(%u));"
                 "proc.beginFrame(3);proc.draw(pb,[]);proc.commit()",n);
        last_malloc=0;
        eval_ok(ctx,source);
        /* 4-byte header + 15 alignment slack + four planes of round8(n). */
        REQUIRE(last_malloc==4u+15u+4u*((n+7u)&~7u)*2u);
        settle(actual);
        int16_t x[128],y[128];
        for(unsigned i=0;i<n;i++){x[i]=(int16_t)(i*2-20);y[i]=(int16_t)(60+(i*37)%50-25);}
        for(unsigned i=0;i<KSN_PROC_W*KSN_PROC_H;i++)expected[i]=3;
        memset(&reference_frame,0,sizeof reference_frame);
        reference_frame.ready=true;reference_frame.count=(uint16_t)(n-1);
        for(unsigned i=1;i<n;i++)
            reference_frame.segments[i-1]=(ksn_proc_segment){x[i-1],y[i-1],x[i],y[i],0x07e0};
        REQUIRE(ksn_proc_render_band(&reference_frame,expected,0,KSN_PROC_H));
        REQUIRE(memcmp(actual,expected,sizeof actual)==0);
        eval_ok(ctx,"proc.unregister(pb)");
        if(n>=8)++*pie_expected;
    }
    /* Largest batch: the new layout is no bigger than the fixed one was
     * (1,040 + 15 B). */
    REQUIRE(4u+15u+4u*128u*2u<=1040u+15u);
}

int main(void){
    JSRuntime *rt=JS_NewRuntime();REQUIRE(rt);
    JSContext *ctx=JS_NewContext(rt);REQUIRE(ctx);
    JSValue global=JS_GetGlobalObject(ctx),pocket=JS_NewObject(ctx),kasane=JS_NewObject(ctx);
    REQUIRE(pocket_proc_install(ctx,kasane)==ESP_OK);
    REQUIRE(JS_SetPropertyStr(ctx,pocket,"kasane",kasane)>=0);
    REQUIRE(JS_SetPropertyStr(ctx,global,"pocket",pocket)>=0);
    REQUIRE(JS_SetPropertyStr(ctx,global,"requestYield",
                              JS_NewCFunction(ctx,request_yield,"requestYield",0))>=0);
    JS_FreeValue(ctx,global);
    eval_ok(ctx,prelude);
    park_contract(ctx);
    leave_contract(ctx);
    limit_contract(ctx);
    unsigned pie=0;
    sizing_contract(ctx,&pie);
    uint32_t scalar_batches=0,pie_batches=0;
    pocket_proc_batch_counts(&scalar_batches,&pie_batches);
#if KSN_PROC_POINTS_HAS_PIE
    REQUIRE(pie_batches==pie&&blocks_run>0);
#else
    REQUIRE(pie_batches==0);
#endif
    pocket_proc_reset();
    JS_FreeContext(ctx);JS_FreeRuntime(rt);
    printf("PASS procedural turn: parked frame commits after 1 and 3 parks, terminated/thrown "
           "frame carries nothing, Back's save frame opens after the finished frame is presented, "
           "limit before allocation, per-count points (%u PIE batches)\n",
           (unsigned)pie_batches);
    return 0;
}
