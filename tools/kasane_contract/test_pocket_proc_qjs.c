/* The frames, surfaces and image resources of pocket_proc.c through JS, on
 * real QuickJS: beginFrame/commit/resource/createSurface and Shiki draws on
 * pocket.kasane.shiki (pocket_shiki_api.c), the image port the presenter
 * reads, transfer failure and repair, two independent surfaces on the real
 * Kasane core and renderer, and stale resources. run_pocket_proc_qjs.py
 * builds it.
 *
 * The frames are drawn by small Shiki programs assembled here as bytes (the
 * sec.3 layout, asm() as apps/shikitest/shiki_api_test.js writes it) with
 * whole-pixel inputs, so the C reference is the same segments rendered by
 * ksn_proc_render_band. The float procedural VM, pocket.kasane.procedural and
 * MEGADEMO's programs, which this harness used to run, went in Shiki v2.0;
 * the app itself is run by run_megademo_app_host.py. */
#include "quickjs.h"
#include "pocket_proc.h"
#include "pocket_api.h"
#include "pocket_shiki_api.h"
#include "shiki_heap.h"
#include "core_fixture.h"
#include "ksn_render.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define REQUIRE(x) do { if(!(x)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);} } while(0)
static uint16_t actual[KSN_PROC_W*KSN_PROC_H], expected[KSN_PROC_W*KSN_PROC_H];
static unsigned publishes,invalidates;
KSN_TEST_CORE(multi_core,static);
static bool multi_active;
static ksn_resource multi_resource[2];
static uint16_t multi_panel[KSN_PROC_W*KSN_PROC_H],multi_strip[KSN_PROC_W*8];
static unsigned multi_bytes;
static int multi_fail_y=-1;
static uint16_t *multi_buffer(void *ctx){(void)ctx;return multi_strip;}
static ksn_result multi_send(void *ctx,uint16_t y,uint16_t rows,const uint16_t *pixels){
    (void)ctx;multi_bytes+=(unsigned)rows*KSN_PROC_W*2;
    memcpy(multi_panel+y*KSN_PROC_W,pixels,(size_t)rows*KSN_PROC_W*2);
    return y==multi_fail_y?KSN_IO:KSN_OK;
}
static ksn_result multi_send_rect(void *ctx,uint16_t x,uint16_t y,uint16_t cols,
                                  uint16_t rows,const uint16_t *pixels){
    (void)ctx;multi_bytes+=(unsigned)rows*cols*2;
    for(unsigned row=0;row<rows;row++)
        memcpy(multi_panel+(y+row)*KSN_PROC_W+x,
               pixels+(size_t)row*KSN_PROC_W+x,cols*2);
    return y==multi_fail_y?KSN_IO:KSN_OK;
}

ksn_result pocket_kasane_proc_publish(void){publishes++;return KSN_OK;}
ksn_result pocket_kasane_proc_publish_at(unsigned surface,ksn_rect damage){
    publishes++;
    if(multi_active&&surface<2&&multi_resource[surface].value)
        ksn_core_invalidate_image_source_rect(&multi_core,multi_resource[surface],damage);
    return KSN_OK;
}
void pocket_kasane_invalidate(void){invalidates++;}
JSValue pocket_kasane_proc_resource(JSContext *ctx){
    pocket_proc_image_mode();
    if(multi_active&&!multi_resource[0].value){
        ksn_image_port port;pocket_proc_image_port_at(&port,0);
        REQUIRE(ksn_core_register_image(&multi_core,KSN_APP,&port,&multi_resource[0])==KSN_OK);
    }
    return JS_NewObject(ctx);
}
JSValue pocket_kasane_proc_resource_at(JSContext *ctx,unsigned surface){
    pocket_proc_image_mode_at(surface);
    if(multi_active&&surface<2&&!multi_resource[surface].value){
        ksn_image_port port;pocket_proc_image_port_at(&port,surface);
        REQUIRE(ksn_core_register_image(&multi_core,KSN_APP,&port,&multi_resource[surface])==KSN_OK);
    }
    return JS_NewObject(ctx);
}
/* The code first in the message, so the scripts below can read it. */
JSValue pocket_api_throw(JSContext *ctx,const char *code,const char *op,
                         const char *message,bool retryable,const char *outcome){
    (void)retryable;(void)outcome;
    return JS_ThrowTypeError(ctx,"%s %s: %s",code,op,message);
}
/* pocket_shiki_api.c's capability registration, never called here. */
esp_err_t pocket_api_register(const pocket_capability_t *capability){(void)capability;return ESP_OK;}
static void eval_ok(JSContext *ctx,const char *source){
    JSValue result=JS_Eval(ctx,source,strlen(source),"adapter-test.js",JS_EVAL_TYPE_GLOBAL);
    if(JS_IsException(result)){
        JSValue error=JS_GetException(ctx);const char *message=JS_ToCString(ctx,error);
        fprintf(stderr,"QuickJS: %s\nwhile running: %s\n",message?message:"exception",source);
        if(message)JS_FreeCString(ctx,message);JS_FreeValue(ctx,error);exit(1);
    }
    JS_FreeValue(ctx,result);
}
static void eval_error(JSContext *ctx,const char *source){
    JSValue result=JS_Eval(ctx,source,strlen(source),"adapter-test.js",JS_EVAL_TYPE_GLOBAL);
    if(!JS_IsException(result))fprintf(stderr,"no error from: %s\n",source);
    REQUIRE(JS_IsException(result));
    JSValue error=JS_GetException(ctx);JS_FreeValue(ctx,error);
}
/* pocket_proc_reset() then pocket_shiki_api_reset(), app_stop()'s order. */
static void session_reset(void){pocket_proc_reset();pocket_shiki_api_reset();}

/* The pixel workload: FRAMES frames of LINES lines, each its own program
 * (a colour each) drawn with four whole-pixel inputs. The same integers in
 * JS (prelude) and here; some lines leave the panel on either side. */
#define FRAMES 24u
#define LINES 5u
static const uint16_t line_color[LINES]={0xf800,0x07e0,0x001f,0xffe0,0xf81f};
static uint16_t frame_backdrop(unsigned f){return (uint16_t)((f*2117u)&0xffffu);}
static void line_ends(unsigned f,unsigned l,int e[4]){
    e[0]=(int)((f*37u+l*53u)%360u)-60;
    e[1]=(int)((f*11u+l*29u)%200u)-30;
    e[2]=(int)((f*23u+l*71u+97u)%360u)-60;
    e[3]=(int)((f*19u+l*41u+50u)%200u)-30;
}
static void reference(unsigned f){
    static ksn_proc_frame frame;
    memset(&frame,0,sizeof frame);
    for(unsigned l=0;l<LINES;l++){
        int e[4];line_ends(f,l,e);
        frame.segments[frame.count++]=(ksn_proc_segment){(int16_t)e[0],(int16_t)e[1],
            (int16_t)e[2],(int16_t)e[3],line_color[l]};
    }
    frame.ready=true;
    const uint16_t color=frame_backdrop(f);
    for(unsigned i=0;i<KSN_PROC_W*KSN_PROC_H;i++)expected[i]=color;
    REQUIRE(ksn_proc_render_band(&frame,expected,0,KSN_PROC_H));
}
static const char prelude[]=
    "globalThis.S=pocket.kasane.shiki;"
    /* sec.3 bytes: regions [type,q,k,write], code [op,dst,a,b,imm,aux]. */
    "globalThis.asm=(regions,code,inputs)=>{inputs=inputs||[];"
    "const head=12+inputs.length+4*regions.length,body=(head+3)&~3;"
    "const b=new Uint8Array(body+12*code.length),dv=new DataView(b.buffer);"
    "b.set([83,75,73,49,1,0,code.length,inputs.length,0,regions.length,0,0]);"
    "b.set(inputs,12);regions.forEach((r,k)=>b.set(r,12+inputs.length+4*k));"
    "code.forEach((i,k)=>{const o=body+12*k;b.set(i.slice(0,4),o);"
    "dv.setInt32(o+4,i[4]||0,true);dv.setUint16(o+8,i[5]||0,true)});return b};"
    /* PLOT at fixed (x, y): SET r0, SET r1 (Q16 pixels), PLOT. */
    "globalThis.dotAt=(x,y,c)=>asm([],[[0,0,0,0,x*65536],[0,1,0,0,y*65536],[8,0,0,1,0,c]]);"
    /* PLOT at the inputs (x, y), Q16. */
    "globalThis.dotIn=c=>asm([],[[1,0,0],[1,1,1],[8,0,0,1,0,c]],[16,16]);"
    /* LINE from (x0, y0) to (x1, y1), all inputs: INPUT x4, MOVE, LINE. */
    "globalThis.lineIn=c=>asm([],[[1,0,0],[1,1,1],[1,2,2],[1,3,3],[7,0,0,1],[9,0,2,3,0,c]],[16,16,16,16]);"
    "globalThis.code=f=>{try{f();return 'OK'}catch(e){const m=String(e);"
    "for(const c of ['INVALID_ARGUMENT','LIMIT_EXCEEDED','CLOSED','BUSY','OUT_OF_MEMORY'])"
    "if(m.includes(c))return c;return m}};"
    "globalThis.expect=(f,c)=>{const got=code(f);"
    "if(got!==c)throw Error('expected '+c+' got '+got+': '+f)};"
    "globalThis.LINES=[0xf800,0x07e0,0x001f,0xffe0,0xf81f].map(c=>S.register(lineIn(c)));"
    "globalThis.ends=(f,l)=>[(f*37+l*53)%360-60,(f*11+l*29)%200-30,"
    "(f*23+l*71+97)%360-60,(f*19+l*41+50)%200-30];"
    "globalThis.frameAt=f=>{S.beginFrame((f*2117)&0xffff);"
    "for(let l=0;l<5;l++)S.draw(LINES[l],...ends(f,l));S.commit()};";

static void check_image_rows(const ksn_image_port *image){
    uint16_t span[KSN_PROC_W];uint8_t alpha[KSN_PROC_W];
    /* Alternating bands, the short final band, and duplicate source rows
     * exercise zoomed/reordered reads independently of sequential scans. */
    static const uint16_t rows[]={134,0,7,8,15,16,128,133,129,134,64,63,64,0};
    for(unsigned i=0;i<sizeof rows/sizeof rows[0];i++){
        uint16_t y=rows[i];
        REQUIRE(image->read_span(image->ctx,0,0,y,3,KSN_PROC_W-3,span,alpha)==KSN_OK);
        REQUIRE(memcmp(span,expected+(unsigned)y*KSN_PROC_W+3,
                       (KSN_PROC_W-3)*sizeof *span)==0);
        for(unsigned x=0;x<KSN_PROC_W-3;x++)REQUIRE(alpha[x]==255);
    }
    for(unsigned y=0;y<KSN_PROC_H;y++){
        /* A two-row source map makes each row appear twice during shrink. */
        for(unsigned repeat=0;repeat<2;repeat++){
            REQUIRE(image->read_span(image->ctx,0,0,y,0,KSN_PROC_W,span,alpha)==KSN_OK);
            REQUIRE(memcmp(span,expected+(unsigned)y*KSN_PROC_W,sizeof span)==0);
        }
    }
}
int main(void){
    /* A Shiki session: the frames, the workspace and the programs come from
     * the Shiki heap at its default budget, as on the device. */
    REQUIRE(shiki_heap_begin(0));
    JSRuntime *rt=JS_NewRuntime();REQUIRE(rt);
    JSContext *ctx=JS_NewContext(rt);REQUIRE(ctx);
    JSValue global=JS_GetGlobalObject(ctx),pocket=JS_NewObject(ctx),kasane=JS_NewObject(ctx);
    REQUIRE(JS_SetPropertyStr(ctx,kasane,"grid",JS_NewObject(ctx))>=0);
    REQUIRE(pocket_shiki_api_install(ctx,kasane)==ESP_OK);
    REQUIRE(JS_SetPropertyStr(ctx,pocket,"kasane",kasane)>=0);
    REQUIRE(JS_SetPropertyStr(ctx,global,"pocket",pocket)>=0);
    JS_FreeValue(ctx,global);
    ksn_image_port image;pocket_proc_image_port(&image);
    REQUIRE(image.width==KSN_PROC_W&&image.height==KSN_PROC_H);
    REQUIRE(image.variants==1&&image.frames==1);
    uint16_t span[KSN_PROC_W];uint8_t alpha[KSN_PROC_W];
    clock_t span_ticks=0;
    REQUIRE(image.read_span(image.ctx,1,0,0,0,1,span,alpha)==KSN_INVALID);
    REQUIRE(image.read_span(image.ctx,0,0,0,0,KSN_PROC_W,span,alpha)==KSN_OK);
    for(unsigned x=0;x<KSN_PROC_W;x++)REQUIRE(span[x]==0&&alpha[x]==255);
    eval_ok(ctx,prelude);
    eval_ok(ctx,"S.resource()");
    REQUIRE(pocket_proc_is_image_mode());

    /* The pixel contract: each frame, read back through the backdrop and the
     * image port, equals the reference; the first two survive two failed
     * transfers before their ACK. */
    publishes=0;
    const unsigned invalidates0=invalidates;
    for(unsigned f=0;f<FRAMES;f++){
        char call[48];snprintf(call,sizeof call,"frameAt(%u)",f);eval_ok(ctx,call);
        REQUIRE(pocket_proc_pending());
        REQUIRE(pocket_proc_backdrop(NULL,0,KSN_PROC_H,actual)==KSN_OK);
        reference(f);
        REQUIRE(memcmp(actual,expected,sizeof actual)==0);
        for(unsigned y=0;y<KSN_PROC_H;y++){
            REQUIRE(image.read_span(image.ctx,0,0,y,0,KSN_PROC_W,span,alpha)==KSN_OK);
            REQUIRE(memcmp(span,expected+y*KSN_PROC_W,sizeof span)==0);
            for(unsigned x=0;x<KSN_PROC_W;x++)REQUIRE(alpha[x]==255);
        }
        clock_t span_start=clock();
        for(unsigned pass=0;pass<10;pass++)for(unsigned y=0;y<KSN_PROC_H;y++)
            REQUIRE(image.read_span(image.ctx,0,0,y,0,KSN_PROC_W,span,alpha)==KSN_OK);
        span_ticks+=clock()-span_start;
        check_image_rows(&image);
        if(f<=1){
            pocket_proc_present_result(KSN_IO);
            REQUIRE(pocket_proc_pending());
            REQUIRE(pocket_proc_backdrop(NULL,0,KSN_PROC_H,actual)==KSN_OK);
            REQUIRE(memcmp(actual,expected,sizeof actual)==0);
            REQUIRE(image.read_span(image.ctx,0,0,25,7,31,span,alpha)==KSN_OK);
            REQUIRE(memcmp(span,expected+25*KSN_PROC_W+7,31*sizeof *span)==0);
            check_image_rows(&image);
            pocket_proc_present_result(KSN_IO); /* repeated transfer failure */
            REQUIRE(pocket_proc_pending());
            REQUIRE(image.read_span(image.ctx,0,0,25,7,31,span,alpha)==KSN_OK);
            REQUIRE(memcmp(span,expected+25*KSN_PROC_W+7,31*sizeof *span)==0);
            check_image_rows(&image);
            /* The image node's core ticket owns IO retry; no backdrop-wide
             * invalidation is needed for its candidate. */
            REQUIRE(invalidates==invalidates0);
            pocket_proc_present_result(KSN_OK); /* UI ticket and image ACK */
            REQUIRE(!pocket_proc_pending());
            REQUIRE(image.read_span(image.ctx,0,0,25,7,31,span,alpha)==KSN_OK);
            REQUIRE(memcmp(span,expected+25*KSN_PROC_W+7,31*sizeof *span)==0);
        }else{
            pocket_proc_present_result(KSN_OK);
            REQUIRE(!pocket_proc_pending());
            REQUIRE(pocket_proc_backdrop(NULL,0,KSN_PROC_H,actual)==KSN_OK);
            REQUIRE(memcmp(actual,expected,sizeof actual)==0);
            REQUIRE(image.read_span(image.ctx,0,0,25,7,31,span,alpha)==KSN_OK);
            REQUIRE(memcmp(span,expected+25*KSN_PROC_W+7,31*sizeof *span)==0);
        }
    }
    REQUIRE(publishes==FRAMES);

    /* A refused program, a failed draw (it closes the frame), and the end of
     * the turn closing a frame. */
    eval_error(ctx,"S.register(new Uint8Array([1,2,3]))");
    eval_ok(ctx,"globalThis.dot=S.register(dotIn(63488));S.beginFrame(0)");
    eval_error(ctx,"S.draw(dot,NaN,0)");
    eval_error(ctx,"S.commit()");
    eval_ok(ctx,"S.beginFrame(0)");
    pocket_proc_end_turn();
    eval_error(ctx,"S.commit()");
    session_reset();
    REQUIRE(!pocket_proc_is_image_mode());
    REQUIRE(image.read_span(image.ctx,0,0,25,7,31,span,alpha)==KSN_OK);
    for(unsigned i=0;i<31;i++)REQUIRE(span[i]==0&&alpha[i]==255);
    /* reset() closed every handle. */
    eval_ok(ctx,"S.beginFrame(0)");
    eval_error(ctx,"S.draw(dot,0,0)");

    /* Backdrop mode (no resource): a failed transfer keeps the committed
     * frame and owes a repair before the next frame opens. */
    eval_ok(ctx,"globalThis.fresh=S.register(dotAt(0,0,1));"
                "S.beginFrame(63488);S.draw(fresh);S.commit()");
    pocket_proc_present_result(KSN_IO);
    REQUIRE(pocket_proc_has_frame());
    REQUIRE(invalidates>0); /* Backdrop mode repairs the old committed frame. */
    REQUIRE(pocket_proc_backdrop(NULL,0,KSN_PROC_H,actual)==KSN_OK);
    for(unsigned i=0;i<KSN_PROC_W*KSN_PROC_H;i++)REQUIRE(actual[i]==0);
    eval_error(ctx,"S.beginFrame(0)"); /* Old backdrop owes repair. */
    pocket_proc_overlay_present_result(KSN_OK); /* FLOWER cannot repair it. */
    eval_error(ctx,"S.beginFrame(0)");
    pocket_proc_present_result(KSN_OK);
    REQUIRE(!pocket_proc_has_frame());
    eval_ok(ctx,"S.beginFrame(0)");
    pocket_proc_end_turn();
    session_reset();

    /* Reentrancy: a getter that runs inside a guarded call (resource()'s
     * option) meets BUSY; one that runs while draw() reads its inputs may
     * close the frame, and then the draw adds nothing to any frame. */
    eval_ok(ctx,
        "let nestedBusy=false;"
        "S.resource({get maxSegments(){try{S.beginFrame(0)}"
        "catch(e){nestedBusy=String(e).includes('BUSY')}return 96}});"
        "if(!nestedBusy)throw Error('resource reentrancy');"
        "globalThis.guarded=S.register(dotAt(5,5,0xffff));"
        "S.beginFrame(7);let guardIn=[0,0];"
        "Object.defineProperty(guardIn,0,{get(){S.commit();return 0}});"
        "expect(()=>S.draw(guarded,guardIn),'BUSY')");
    REQUIRE(pocket_proc_pending());
    REQUIRE(pocket_proc_backdrop(NULL,0,KSN_PROC_H,actual)==KSN_OK);
    for(unsigned i=0;i<KSN_PROC_W*KSN_PROC_H;i++)REQUIRE(actual[i]==7);
    pocket_proc_present_result(KSN_OK);
    session_reset();

    /* Inputs: a different input draws a different frame; the numeric form
     * draws what the array form draws, short lists zero-padded, trailing
     * undefined arguments not passed. */
    eval_ok(ctx,"globalThis.curve=S.register(asm([],[[0,0,0,0,10*65536],[0,1,0,0,10*65536],"
                "[7,0,0,1],[1,2,0],[0,3,0,0,100*65536],[9,0,2,3,0,2016]],[16]));"
                "S.beginFrame(0);S.draw(curve,[95]);S.commit()");
    REQUIRE(pocket_proc_pending());
    REQUIRE(pocket_proc_backdrop(NULL,0,KSN_PROC_H,actual)==KSN_OK);
    REQUIRE(actual[10*KSN_PROC_W+10]==0x07e0);
    REQUIRE(actual[100*KSN_PROC_W+95]==0x07e0);
    static uint16_t previous[KSN_PROC_W*KSN_PROC_H];
    memcpy(previous,actual,sizeof previous);
    pocket_proc_present_result(KSN_OK);
    eval_ok(ctx,"S.beginFrame(0);S.draw(curve,[25]);S.commit()");
    REQUIRE(pocket_proc_pending());
    REQUIRE(pocket_proc_backdrop(NULL,0,KSN_PROC_H,actual)==KSN_OK);
    REQUIRE(memcmp(previous,actual,sizeof actual)!=0);
    pocket_proc_present_result(KSN_OK);
    static const char *const SAME_AS_95[]={
        "S.draw(curve,95,0,0,0)","S.draw(curve,95)",
        "S.draw(curve,95,0,undefined,undefined)","S.draw(curve,[95,0,0,0],undefined)",
        "S.draw(curve,[95],undefined,undefined)"};
    for(unsigned i=0;i<sizeof SAME_AS_95/sizeof SAME_AS_95[0];i++){
        char js[160];
        snprintf(js,sizeof js,"S.beginFrame(0);%s;S.commit()",SAME_AS_95[i]);
        eval_ok(ctx,js);
        REQUIRE(pocket_proc_pending());
        REQUIRE(pocket_proc_backdrop(NULL,0,KSN_PROC_H,actual)==KSN_OK);
        REQUIRE(memcmp(previous,actual,sizeof actual)==0);
        pocket_proc_present_result(KSN_OK);
    }
    /* Shiki converts only the inputs a program declares (curve has one), so
     * a non-finite value is refused in that position; a non-number is
     * refused in any. draw(h) alone is zero inputs, padded, and valid. */
    static const char *const BAD_NUMERIC[]={
        "S.draw(curve,95,undefined,0)",       /* a hole */
        "S.draw(curve,1,2,3,4,5,6,7,8,9)",    /* nine inputs */
        "S.draw(curve,NaN)","S.draw(curve,Infinity)","S.draw(curve,1e39)",
        "S.draw(curve,95,'1')","S.draw(curve,'95')","S.draw(curve,[95],0)",
        "S.draw(curve,undefined)","S.draw(999999,95)"};
    for(unsigned i=0;i<sizeof BAD_NUMERIC/sizeof BAD_NUMERIC[0];i++){
        eval_ok(ctx,"S.beginFrame(0)");
        eval_error(ctx,BAD_NUMERIC[i]);
    }
    eval_ok(ctx,"S.beginFrame(0);let code='';try{S.draw(999999,95)}catch(e){code=e.message}"
                "if(!code.startsWith('CLOSED'))throw Error('stale handle: '+code);"
                "S.beginFrame(0);try{S.draw(curve,95,undefined,0)}catch(e){code=e.message}"
                "if(!code.startsWith('INVALID_ARGUMENT'))throw Error('hole: '+code)");
    pocket_proc_end_turn();
    session_reset();

    /* Two image resources keep independent committed frames and reject a
     * second writer until the first candidate's display ticket is retired.
     * The second surface is capped (createSurface({maxSegments})) so both fit
     * the default Shiki heap, as an app's would. */
    ksn_core_init(&multi_core);multi_active=true;
    eval_ok(ctx,
        "globalThis.layerId=S.createSurface({maxSegments:256});"
        "if(!Number.isInteger(layerId)||layerId<=0)throw Error('surface ID');"
        "S.beginFrame(0,layerId)");
    eval_error(ctx,"S.commit()"); /* An extra surface needs its image lease. */
    pocket_proc_end_turn();
    eval_ok(ctx,"S.resource();S.resource(layerId);globalThis.dotPlan=S.register(dotIn(63488))");
    ksn_client app=ksn_core_client(&multi_core,KSN_APP);
    ksn_tx tx;ksn_ref refs[2];
    REQUIRE(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    REQUIRE(app.ops->background(app.ctx,tx,0x001f00ff)==KSN_OK);
    for(unsigned i=0;i<2;i++){
        int x=i?80:0;
        ksn_draw draw={.kind=KSN_IMAGE,.bounds={(int16_t)x,0,(int16_t)(x+64),48},
                       .clip={0,0,240,135},.opacity=255,
                       .data.image={.resource=multi_resource[i],.source_width=64,
                                    .source_height=48,.scale=KSN_IMAGE_1X}};
        REQUIRE(app.ops->add(app.ctx,tx,&draw,&refs[i])==KSN_OK);
    }
    REQUIRE(app.ops->end(app.ctx,tx)==KSN_OK);
    ksn_display_port multi_display={.strip=multi_buffer,.present=multi_send,
        .present_rect=multi_send_rect,.width=240,.height=135,.strip_rows=8};
    ksn_render_stats multi_stats;
    REQUIRE(ksn_render_rects(&multi_core,&multi_display,&multi_stats)==KSN_OK);
    ksn_image_port first,second;
    pocket_proc_image_port_at(&first,0);pocket_proc_image_port_at(&second,1);
    REQUIRE(first.ctx!=second.ctx);
    eval_ok(ctx,"S.beginFrame(0);S.draw(dotPlan,10,10);S.commit()");
    eval_error(ctx,"S.beginFrame(0,layerId)");
    multi_bytes=0;
    REQUIRE(ksn_render_rects(&multi_core,&multi_display,&multi_stats)==KSN_OK);
    REQUIRE(multi_bytes==64*48*2&&multi_panel[10*240+10]==0xf800);
    pocket_proc_overlay_present_result(KSN_OK);
    eval_ok(ctx,"S.beginFrame(0,layerId);S.draw(dotPlan,20,10);S.commit()");
    REQUIRE(second.read_span(second.ctx,0,0,10,20,1,span,alpha)==KSN_OK);
    REQUIRE(span[0]==0xf800);
    REQUIRE(first.read_span(first.ctx,0,0,10,10,1,span,alpha)==KSN_OK);
    REQUIRE(span[0]==0xf800);
    multi_bytes=0;
    REQUIRE(ksn_render_rects(&multi_core,&multi_display,&multi_stats)==KSN_OK);
    REQUIRE(multi_bytes==64*48*2&&multi_panel[10*240+100]==0xf800);
    pocket_proc_overlay_present_result(KSN_OK);
    REQUIRE(!pocket_proc_pending());
    /* One list a surface (shiki-jit.md sec.23): while a frame is built, a
     * read of that surface shows the list as it is (beginFrame started it
     * over); the other surface is untouched. The image band cache must not
     * serve a stale row on either. Both phase orders. */
    for(unsigned target=0;target<2;target++){
        REQUIRE(first.read_span(first.ctx,0,0,10,10,1,span,alpha)==KSN_OK&&span[0]==0xf800);
        eval_ok(ctx,target?"S.beginFrame(0,layerId);S.draw(dotPlan,42,10)":
                           "S.beginFrame(0);S.draw(dotPlan,42,10)");
        REQUIRE(first.read_span(first.ctx,0,0,10,10,1,span,alpha)==KSN_OK&&
                span[0]==(target?0xf800:0));
        REQUIRE(first.read_span(first.ctx,0,0,10,42,1,span,alpha)==KSN_OK&&
                span[0]==(target?0:0xf800));
        REQUIRE(second.read_span(second.ctx,0,0,10,20,1,span,alpha)==KSN_OK&&
                span[0]==(target?0:0xf800));
        REQUIRE(first.read_span(first.ctx,0,0,8,0,KSN_PROC_W,span,alpha)==KSN_OK);
        for(unsigned x=0;x<KSN_PROC_W;x++)REQUIRE(span[x]==0&&alpha[x]==255);
        pocket_proc_end_turn();
        /* Shown again as it was, for the next round and what follows. */
        eval_ok(ctx,target?"S.beginFrame(0,layerId);S.draw(dotPlan,20,10);S.commit()":
                           "S.beginFrame(0);S.draw(dotPlan,10,10);S.commit()");
        multi_bytes=0;
        REQUIRE(ksn_render_rects(&multi_core,&multi_display,&multi_stats)==KSN_OK);
        pocket_proc_overlay_present_result(KSN_OK);
        REQUIRE(!pocket_proc_pending());
        REQUIRE(second.read_span(second.ctx,0,0,10,20,1,span,alpha)==KSN_OK&&span[0]==0xf800);
    }
    REQUIRE(first.read_span(first.ctx,0,0,10,10,1,span,alpha)==KSN_OK&&span[0]==0xf800);
    /* One past the coordinate range (720) fails; the list beginFrame()
     * started over stays empty (the failed draw adds nothing). */
    eval_ok(ctx,"S.beginFrame(0)");
    eval_error(ctx,"S.draw(dotPlan,721,10)");
    REQUIRE(first.read_span(first.ctx,0,0,10,10,1,span,alpha)==KSN_OK&&span[0]==0);
    pocket_proc_end_turn();
    eval_ok(ctx,"S.beginFrame(0);S.draw(dotPlan,11,10);S.commit()");
    REQUIRE(second.read_span(second.ctx,0,0,10,20,1,span,alpha)==KSN_OK&&span[0]==0xf800);
    multi_bytes=0;
    REQUIRE(ksn_render_rects(&multi_core,&multi_display,&multi_stats)==KSN_OK);
    REQUIRE(multi_bytes==16*8*2&&multi_panel[10*240+10]==0&&
            multi_panel[10*240+11]==0xf800&&multi_panel[10*240+100]==0xf800);
    pocket_proc_overlay_present_result(KSN_OK);
    eval_ok(ctx,"S.beginFrame(0,layerId);S.draw(dotPlan,21,10);S.commit()");
    multi_fail_y=8;
    REQUIRE(ksn_render_rects(&multi_core,&multi_display,&multi_stats)==KSN_IO);
    pocket_proc_overlay_present_result(KSN_IO);
    REQUIRE(pocket_proc_pending());
    multi_fail_y=-1;multi_bytes=0;
    REQUIRE(ksn_render_rects(&multi_core,&multi_display,&multi_stats)==KSN_OK);
    REQUIRE(multi_bytes==240*135*2&&multi_panel[10*240+100]==0&&
            multi_panel[10*240+101]==0xf800&&multi_panel[10*240+11]==0xf800);
    pocket_proc_overlay_present_result(KSN_OK);
    REQUIRE(!pocket_proc_pending());
    eval_error(ctx,"S.createSurface()");
    REQUIRE(ksn_core_reset_layer(&multi_core,KSN_APP)==KSN_OK);
    ksn_image_port stale_port;
    REQUIRE(ksn_core_image_port(&multi_core,KSN_APP,multi_resource[0],&stale_port)==KSN_STALE);
    REQUIRE(ksn_core_image_port(&multi_core,KSN_APP,multi_resource[1],&stale_port)==KSN_STALE);
    multi_active=false;
    session_reset();
    eval_error(ctx,"S.resource(layerId)");
    eval_ok(ctx,"const newLayerId=S.createSurface();"
                "if(newLayerId===layerId)throw Error('stale surface ID reused')");
    session_reset();
    JS_FreeContext(ctx);JS_FreeRuntime(rt);
    shiki_heap_end();
    uint32_t bytes,used,largest;shiki_heap_info(&bytes,&used,&largest);
    REQUIRE(!bytes);
    printf("PASS real QuickJS frame adapter (pocket.kasane.shiki): %u exact RGB565 frames, "
           "two independent image surfaces, 256 B rectangle damage, IO repair, stale resources; "
           "image span CPU %.1f ms\n",FRAMES,1000.0*(double)span_ticks/CLOCKS_PER_SEC);
    return 0;
}
