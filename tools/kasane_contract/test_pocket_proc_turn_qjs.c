/* Real QuickJS (with the L2c VM) contract for the frame across a turn
 * boundary, the handle limit's precedence over allocation, per-count plan
 * allocation and the per-surface segment cap, through pocket.kasane.shiki.
 * Built by run_pocket_proc_turn_qjs.py under ASan/UBSan with
 * proc_alloc_hook.h force-included into these sources only, so
 * pocket_proc.c's and the Shiki API's allocations can be failed and measured
 * while the prebuilt engine allocates normally. No Shiki heap is opened, so
 * those allocations reach the hooked malloc/calloc one by one.
 *
 * The draws are tiny Shiki programs assembled here as bytes (asm(), the
 * sec.3 layout of apps/shikitest/shiki_api_test.js): the float VM's plans
 * and typed point batches these contracts were first written with went in
 * Shiki v2.0, and so did the point-batch sizing contract.
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
#include "pocket_shiki_api.h"
#include "ksn_shiki.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define REQUIRE(x) do { if(!(x)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);} } while(0)
static uint16_t actual[KSN_PROC_W*KSN_PROC_H];

static bool fail_alloc;
static size_t last_malloc;
void *proc_test_malloc(size_t size){
    if(fail_alloc)return NULL;
    last_malloc=size;return (malloc)(size);
}
static size_t callocs[8];
static unsigned calloc_n;
void *proc_test_calloc(size_t count,size_t size){
    if(fail_alloc)return NULL;
    if(calloc_n<8)callocs[calloc_n]=count*size;
    calloc_n++;
    return (calloc)(count,size);
}

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
esp_err_t pocket_api_register(const pocket_capability_t *capability){(void)capability;return ESP_OK;}
/* pocket_proc_reset() then pocket_shiki_api_reset(), app_stop()'s order: the
 * API's handle book must forget what reset() closed. */
static void session_reset(void){pocket_proc_reset();pocket_shiki_api_reset();}
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
    "globalThis.proc=pocket.kasane.shiki;"
    "globalThis.code=f=>{try{const r=f();return r===undefined?'OK':'VALUE'}catch(e){"
    "const m=String(e);for(const c of ['INVALID_ARGUMENT','LIMIT_EXCEEDED','CLOSED','BUSY',"
    "'OUT_OF_MEMORY'])if(m.includes(c))return c;return m}};"
    "globalThis.expect=(f,c)=>{const got=code(f);"
    "if(got!==c)throw Error('expected '+c+' got '+got+': '+f)};"
    /* The error's text, for two failures that share a code. */
    "globalThis.says=(f,text)=>{try{f()}catch(e){if(String(e).includes(text))return;"
    "throw Error('expected '+text+' got '+e)}throw Error('no error: '+f)};"
    "globalThis.asm=(regions,code,inputs)=>{inputs=inputs||[];"
    "const head=12+inputs.length+4*regions.length,body=(head+3)&~3;"
    "const b=new Uint8Array(body+12*code.length),dv=new DataView(b.buffer);"
    "b.set([83,75,73,49,1,0,code.length,inputs.length,0,regions.length,0,0]);"
    "b.set(inputs,12);regions.forEach((r,k)=>b.set(r,12+inputs.length+4*k));"
    "code.forEach((i,k)=>{const o=body+12*k;b.set(i.slice(0,4),o);"
    "dv.setInt32(o+4,i[4]||0,true);dv.setUint16(o+8,i[5]||0,true)});return b};"
    /* SET r0 = x, SET r1 = y (Q16 pixels), PLOT in colour c. */
    "globalThis.dotRows=(x,y,c)=>[[0,0,0,0,x*65536],[0,1,0,0,y*65536],[8,0,0,1,0,c]];"
    "globalThis.dotAt=(x,y,c)=>asm([],dotRows(x,y,c));"
    /* n PLOTs at (x, 7), x 5 unless given: SET, SET, REPEAT n { PLOT } END. */
    "globalThis.plots=(n,c,x=5)=>asm([],[[0,0,0,0,x*65536],[0,1,0,0,7*65536],[5,0,0,0,n],[8,0,0,1,0,c],[6]]);"
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
    session_reset();
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
    session_reset();
}

/* Bug 2: the 33rd register() is LIMIT_EXCEEDED for the handles before any
 * byte is allocated, so a low heap cannot turn it into a heap failure. Shiki
 * reports both as LIMIT_EXCEEDED (spec sec.9.4: the budget decides at once),
 * so the message tells them apart. */
static void limit_contract(JSContext *ctx){
    eval_ok(ctx,
        "globalThis.hs=[];for(let i=0;i<32;i++)hs.push(proc.register(dotAt(i,i,i+1)));"
        "says(()=>proc.register(dotAt(0,0,1)),'32 handles')");
    fail_alloc=true;
    eval_ok(ctx,"says(()=>proc.register(dotAt(0,0,1)),'32 handles')");
    fail_alloc=false;
    /* With a handle free, the same failure is the heap's and says so. */
    eval_ok(ctx,"proc.unregister(hs[7])");
    fail_alloc=true;
    eval_ok(ctx,"says(()=>proc.register(dotAt(0,0,1)),'budget')");
    fail_alloc=false;
    eval_ok(ctx,
        "hs[7]=proc.register(dotAt(0,0,1));"
        "says(()=>proc.register(dotAt(0,0,1)),'32 handles')");
    session_reset();
}

/* A plan allocates for its own instruction count (ksn_shiki_plan_bytes, not
 * KSN_SHIKI_PLAN_MAX). Each size draws, and a churn of mixed sizes through
 * all 32 handles runs under ASan, so a plan that reads or writes past its own
 * count fails here rather than on the device's shared heap. */
static void plan_size_contract(JSContext *ctx){
    static const unsigned counts[]={1,2,3,8,16,32,63,64};
    eval_ok(ctx,"globalThis.sized=(n,x,y,c)=>{if(n===1)return asm([],[[8,0,0,1,0,c]]);"
                "if(n===2)return asm([],[[0,0,0,0,x*65536],[8,0,0,1,0,c]]);"
                "const p=[];for(let i=0;i<n-3;i++)p.push([0,2,0,0,0]);"
                "return asm([],p.concat(dotRows(x,y,c)))}");
    for(unsigned k=0;k<sizeof counts/sizeof counts[0];k++){
        const unsigned n=counts[k];
        char source[200];
        snprintf(source,sizeof source,
                 "globalThis.ps=proc.register(sized(%u,40,50,0xffff))",n);
        last_malloc=0;
        eval_ok(ctx,source);
        REQUIRE(last_malloc==ksn_shiki_plan_bytes(n));
        REQUIRE(n==KSN_SHIKI_CODE||last_malloc<KSN_SHIKI_PLAN_MAX);
        eval_ok(ctx,"proc.beginFrame(3);proc.draw(ps,[]);proc.commit();proc.unregister(ps)");
        settle(actual);
        const unsigned at=n==1?0:n==2?40:50*KSN_PROC_W+40;
        REQUIRE(actual[at]==0xffff&&count_color(actual,0xffff)==1);
    }
    eval_ok(ctx,"expect(()=>proc.register(sized(65,0,0,1)),'INVALID_ARGUMENT')");
    eval_ok(ctx,"globalThis.sd=12345;globalThis.rnd=m=>(sd=(sd*1103515245+12345)&0x7fffffff)%m;"
                "globalThis.live=new Array(32).fill(0)");
    for(unsigned round=0;round<12;round++){
        eval_ok(ctx,
            "for(let it=0;it<50;it++){const i=rnd(32);"
            "if(live[i]){proc.unregister(live[i]);live[i]=0}"
            "else live[i]=proc.register(sized(1+rnd(64),rnd(240),rnd(135),1+rnd(65535)))}"
            "proc.beginFrame(0);for(const h of live)if(h)proc.draw(h,[]);proc.commit()");
        settle(actual);
    }
    eval_ok(ctx,
        "for(let i=0;i<32;i++)if(!live[i])live[i]=proc.register(sized(64,i,i,1));"
        "expect(()=>proc.register(sized(1,0,0,1)),'LIMIT_EXCEEDED');"
        "proc.beginFrame(0);for(const h of live)proc.draw(h,[]);proc.commit()");
    settle(actual);
    pocket_proc_end_turn();
    session_reset();
}

/* Per-surface segment cap: createSurface({maxSegments}) sizes that surface's
 * two frames to header + n entries, and draw() enforces n exactly as it
 * enforces 1,024 on surface 0. ASan fails the run if any path (draw's copy,
 * damage, image spans, the present swap) touches an entry past
 * the short allocation; LeakSanitizer checks reset() frees both frames. */
static void cap_contract(JSContext *ctx){
    const size_t head=offsetof(ksn_proc_frame,segments);
    const size_t full=sizeof(ksn_proc_frame);
    REQUIRE(head==6u&&sizeof(ksn_proc_segment)==10u&&full==10246u);
    session_reset(); /* plan_size_contract left surface 0's buffers allocated */
    ksn_image_port image;pocket_proc_image_port(&image);
    uint16_t span[KSN_PROC_W];uint8_t alpha[KSN_PROC_W];
    fail_alloc=true;calloc_n=0;
    REQUIRE(image.read_span(image.ctx,0,0,134,0,KSN_PROC_W,span,alpha)==KSN_OK);
    for(unsigned i=0;i<KSN_PROC_W;i++)REQUIRE(span[i]==0&&alpha[i]==255);
    REQUIRE(calloc_n==0);fail_alloc=false;
    /* 128 PLOTs per draw: SET r0,r1; REPEAT 128 { PLOT } END. */
    eval_ok(ctx,
        "globalThis.p128=proc.register(plots(128,0xffe0));"
        "globalThis.dot=proc.register(dotAt(40,40,0xf800));"
        "globalThis.p127=proc.register(plots(127,0x07e0,9));"
        /* Options are refused before any surface exists. */
        "for(const bad of [0,null,'x',{maxSegments:0},{maxSegments:1025},"
        "{maxSegments:1.5},{maxSegments:'256'},{maxSegments:-1},{maxSegments:NaN}])"
        "expect(()=>proc.createSurface(bad),'INVALID_ARGUMENT');"
        "expect(()=>proc.createSurface({maxSegments:8},1),'INVALID_ARGUMENT')");
    /* Surface 0: two full frames and nothing else -- draws go straight into
     * the candidate, no scratch frame (shiki-jit.md sec.22); the engine's
     * state comes with the first draw. */
    calloc_n=0;
    eval_ok(ctx,"proc.beginFrame(0)");
    REQUIRE(calloc_n==2&&callocs[0]==full&&callocs[1]==full);
    eval_ok(ctx,
        "for(let i=0;i<8;i++)proc.draw(p128,[]);"
        "expect(()=>proc.draw(dot,[]),'LIMIT_EXCEEDED')");
    pocket_proc_end_turn();
    eval_ok(ctx,"proc.beginFrame(0);proc.draw(dot,[]);proc.commit()");
    settle(actual);
    REQUIRE(actual[40*KSN_PROC_W+40]==0xf800);

    /* Surface 1 at 256: two frames of 6 + 2,560 B, nothing else allocated. */
    eval_ok(ctx,"globalThis.small=proc.createSurface({maxSegments:256});proc.resource(small)");
    fail_alloc=true;
    eval_ok(ctx,"expect(()=>proc.beginFrame(0,small),'OUT_OF_MEMORY')");
    fail_alloc=false;
    calloc_n=0;
    eval_ok(ctx,"proc.beginFrame(0,small)");
    REQUIRE(calloc_n==2&&callocs[0]==head+256u*10u&&callocs[1]==head+256u*10u);
    /* Exactly full across three programs (128 + 127 + 1 = 256), then
     * one more is refused and the frame is closed. */
    eval_ok(ctx,
        "proc.draw(p128,[]);proc.draw(p127,[]);proc.draw(dot,[]);"
        "expect(()=>proc.draw(dot,[]),'LIMIT_EXCEEDED');"
        "expect(()=>proc.commit(),'BUSY')");
    pocket_proc_end_turn();
    /* A refused draw closes the frame; a new one fills and commits. */
    eval_ok(ctx,
        "proc.beginFrame(0,small);proc.draw(p128,[]);proc.draw(p128,[]);"
        "expect(()=>proc.draw(dot,[]),'LIMIT_EXCEEDED');"
        "proc.beginFrame(0,small);proc.draw(p128,[]);proc.draw(p127,[]);proc.draw(dot,[]);"
        "proc.commit()");
    ksn_image_port port;pocket_proc_image_port_at(&port,1);
    REQUIRE(port.read_span(port.ctx,0,0,40,0,KSN_PROC_W,span,alpha)==KSN_OK);
    REQUIRE(span[40]==0xf800);
    REQUIRE(port.read_span(port.ctx,0,0,7,0,KSN_PROC_W,span,alpha)==KSN_OK);
    REQUIRE(span[5]==0xffe0&&span[9]==0x07e0);
    pocket_proc_present_result(KSN_OK);
    REQUIRE(!pocket_proc_pending());
    /* Presented, swapped, and drawn again into the other short frame. */
    calloc_n=0;
    eval_ok(ctx,"proc.beginFrame(0,small);proc.draw(dot,[]);proc.commit()");
    REQUIRE(calloc_n==0);
    REQUIRE(port.read_span(port.ctx,0,0,7,0,KSN_PROC_W,span,alpha)==KSN_OK);
    REQUIRE(span[5]==0);
    pocket_proc_present_result(KSN_OK);
    /* Surface 0 keeps its own 1,024 while surface 1 is capped. */
    eval_ok(ctx,
        "proc.beginFrame(0);for(let i=0;i<8;i++)proc.draw(p128,[]);proc.commit()");
    settle(actual);
    eval_ok(ctx,"expect(()=>proc.createSurface({maxSegments:8}),'LIMIT_EXCEEDED')");
    session_reset();
    /* Bounds of the option: 1 and 1,024; {} and undefined mean 1,024. */
    static const struct {const char *create;unsigned segments;} cases[]={
        {"proc.createSurface({maxSegments:1})",1},
        {"proc.createSurface({maxSegments:1024})",1024},
        {"proc.createSurface({})",1024},
        {"proc.createSurface(undefined)",1024},
        {"proc.createSurface()",1024},
    };
    for(unsigned k=0;k<sizeof cases/sizeof cases[0];k++){
        char source[200];
        snprintf(source,sizeof source,
                 "globalThis.dot=proc.register(dotAt(1,1,1));"
                 "globalThis.s=%s;proc.resource(s);proc.beginFrame(0)",cases[k].create);
        eval_ok(ctx,source);
        pocket_proc_end_turn();
        calloc_n=0;
        eval_ok(ctx,"proc.beginFrame(0,s)");
        REQUIRE(calloc_n==2&&callocs[0]==head+cases[k].segments*10u);
        snprintf(source,sizeof source,
                 "for(let i=0;i<%u;i++)proc.draw(dot,[]);"
                 "expect(()=>proc.draw(dot,[]),'LIMIT_EXCEEDED')",cases[k].segments);
        eval_ok(ctx,source);
        pocket_proc_end_turn();
        session_reset();
    }
    /* Surface 0 through resource({maxSegments}): its two frames shrink (no
     * scratch frame since sec.22; the image band cache came with resource());
     * draw() enforces the cap; once the frames exist only the same value is
     * accepted. */
    eval_ok(ctx,
        "for(const bad of [{maxSegments:0},{maxSegments:1025},{maxSegments:1.5},"
        "{maxSegments:'8'},{maxSegments:null}])"
        "expect(()=>proc.resource(bad),'INVALID_ARGUMENT');"
        "proc.resource({maxSegments:96});"
        "globalThis.p32=proc.register(plots(32,0xffe0));"
        "globalThis.dot=proc.register(dotAt(40,40,0xf800))");
    calloc_n=0;
    eval_ok(ctx,"proc.beginFrame(0)");
    REQUIRE(calloc_n==2&&callocs[0]==head+96u*10u&&callocs[1]==head+96u*10u);
    eval_ok(ctx,
        "for(let i=0;i<3;i++)proc.draw(p32,[]);"
        "expect(()=>proc.draw(dot,[]),'LIMIT_EXCEEDED');"
        "expect(()=>proc.resource({maxSegments:200}),'BUSY');"
        "expect(()=>proc.resource({}),'BUSY');"
        "proc.resource({maxSegments:96})");
    pocket_proc_end_turn();
    eval_ok(ctx,"proc.beginFrame(0);proc.draw(dot,[]);proc.commit()");
    settle(actual);
    REQUIRE(actual[40*KSN_PROC_W+40]==0xf800);
    session_reset();
    /* reset() forgets the cap: the next frames are full again. */
    calloc_n=0;
    eval_ok(ctx,"proc.beginFrame(0)");
    REQUIRE(calloc_n==2&&callocs[0]==full&&callocs[1]==full);
    pocket_proc_end_turn();
    session_reset();
}

int main(void){
    JSRuntime *rt=JS_NewRuntime();REQUIRE(rt);
    JSContext *ctx=JS_NewContext(rt);REQUIRE(ctx);
    JSValue global=JS_GetGlobalObject(ctx),pocket=JS_NewObject(ctx),kasane=JS_NewObject(ctx);
    REQUIRE(JS_SetPropertyStr(ctx,kasane,"grid",JS_NewObject(ctx))>=0);
    REQUIRE(pocket_shiki_api_install(ctx,kasane)==ESP_OK);
    REQUIRE(JS_SetPropertyStr(ctx,pocket,"kasane",kasane)>=0);
    REQUIRE(JS_SetPropertyStr(ctx,global,"pocket",pocket)>=0);
    REQUIRE(JS_SetPropertyStr(ctx,global,"requestYield",
                              JS_NewCFunction(ctx,request_yield,"requestYield",0))>=0);
    JS_FreeValue(ctx,global);
    eval_ok(ctx,prelude);
    park_contract(ctx);
    leave_contract(ctx);
    limit_contract(ctx);
    plan_size_contract(ctx);
    cap_contract(ctx);
    session_reset();
    JS_FreeContext(ctx);JS_FreeRuntime(rt);
    printf("PASS procedural turn: parked frame commits after 1 and 3 parks, terminated/thrown "
           "frame carries nothing, Back's save frame opens after the finished frame is presented, "
           "handle limit before allocation, per-count plans, per-surface segment cap\n");
    return 0;
}
