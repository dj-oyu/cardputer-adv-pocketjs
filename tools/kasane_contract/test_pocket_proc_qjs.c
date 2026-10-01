#include "quickjs.h"
#include "pocket_proc.h"
#include "pocket_api.h"
#include "proc_megademo.h"
#include "ksn_proc_plan.h"
#include "core_fixture.h"
#include "ksn_render.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define REQUIRE(x) do { if(!(x)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);} } while(0)
static uint16_t actual[KSN_PROC_W*KSN_PROC_H], expected[KSN_PROC_W*KSN_PROC_H];
static uint16_t previous[KSN_PROC_W*KSN_PROC_H];
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
#ifdef KSN_PROC_HOST_FAKE_PIE
void ksn_proc_points_affine_pie(KsnProcPointDst dst,KsnProcPointSrc src,
                                size_t n,const KsnProcAffineQ14 *coeff){
    ksn_proc_points_affine_scalar(dst,src,n,coeff);
}
#endif

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
    /* Marked so the mock view can tell surface 0's image from the others. */
    JSValue object=JS_NewObject(ctx);
    JS_SetPropertyStr(ctx,object,"__s0",JS_TRUE);
    return object;
}
JSValue pocket_kasane_proc_resource_at(JSContext *ctx,unsigned surface){
    pocket_proc_image_mode_at(surface);
    if(multi_active&&surface<2&&!multi_resource[surface].value){
        ksn_image_port port;pocket_proc_image_port_at(&port,surface);
        REQUIRE(ksn_core_register_image(&multi_core,KSN_APP,&port,&multi_resource[surface])==KSN_OK);
    }
    return JS_NewObject(ctx);
}
JSValue pocket_api_throw(JSContext *ctx,const char *code,const char *op,
                         const char *message,bool retryable,const char *outcome){
    (void)retryable;(void)outcome;
    return JS_ThrowTypeError(ctx,"%s %s: %s",code,op,message);
}
static char *read_file(const char *path,size_t *size){
    FILE *f=fopen(path,"rb");REQUIRE(f);
    REQUIRE(fseek(f,0,SEEK_END)==0);long n=ftell(f);REQUIRE(n>0);
    REQUIRE(fseek(f,0,SEEK_SET)==0);
    char *data=malloc((size_t)n+1);REQUIRE(data);
    REQUIRE(fread(data,1,(size_t)n,f)==(size_t)n);REQUIRE(fclose(f)==0);
    data[n]=0;*size=(size_t)n;return data;
}
static void eval_ok(JSContext *ctx,const char *source){
    JSValue result=JS_Eval(ctx,source,strlen(source),"adapter-test.js",JS_EVAL_TYPE_GLOBAL);
    if(JS_IsException(result)){
        JSValue error=JS_GetException(ctx);const char *message=JS_ToCString(ctx,error);
        fprintf(stderr,"QuickJS: %s\n",message?message:"exception");
        if(message)JS_FreeCString(ctx,message);JS_FreeValue(ctx,error);exit(1);
    }
    JS_FreeValue(ctx,result);
}
static void eval_error(JSContext *ctx,const char *source){
    JSValue result=JS_Eval(ctx,source,strlen(source),"adapter-test.js",JS_EVAL_TYPE_GLOBAL);
    REQUIRE(JS_IsException(result));
    JSValue error=JS_GetException(ctx);JS_FreeValue(ctx,error);
}
static void reference(unsigned tick){
    uint16_t color=proc_mega_backdrop(tick);
    for(unsigned i=0;i<KSN_PROC_W*KSN_PROC_H;i++)expected[i]=color;
    for(unsigned layer=0;layer<PROC_MEGA_LAYERS;layer++){
        ksn_proc_inst code[KSN_PROC_CODE];ksn_proc_program program;
        float inputs[KSN_PROC_INPUTS];ksn_proc_plan plan;
        ksn_proc_vm vm;ksn_proc_frame frame;
        REQUIRE(proc_mega_build(tick,layer,code,&program,inputs));
        REQUIRE(ksn_proc_plan_prepare(&plan,&program));
        REQUIRE(ksn_proc_plan_begin(&vm,&plan,inputs,&frame)==KSN_PROC_RUNNING);
        REQUIRE(ksn_proc_plan_run(&vm,&plan,false)==KSN_PROC_DONE);
        REQUIRE(ksn_proc_render_band(&frame,expected,0,KSN_PROC_H));
        if(layer==3){
            static const KsnProcAffineQ14 coeff[3]={
                {16384,0,0,16384,0,0},
                {16000,1600,-1000,16384,-6*16384,6*16384},
                {15360,-2304,1300,15500,18*16384,-10*16384}};
            static const uint16_t colors[3]={0x07ff,0xfde0,0xf81f};
            _Alignas(16) int16_t x[40],y[40],out_x[40],out_y[40];
            for(unsigned i=0;i<40;i++){
                x[i]=(int16_t)(18+i*5);
                y[i]=(int16_t)(92+((i*13)%23)-11);
            }
            unsigned phase=tick/16;
            ksn_proc_points_affine_scalar((KsnProcPointDst){out_x,out_y},
                (KsnProcPointSrc){x,y},40,&coeff[phase]);
            ksn_proc_frame typed={.ready=true,.count=39};
            for(unsigned i=1;i<40;i++)
                typed.segments[i-1]=(ksn_proc_segment){out_x[i-1],out_y[i-1],
                    out_x[i],out_y[i],colors[phase]};
            REQUIRE(ksn_proc_render_band(&typed,expected,0,KSN_PROC_H));
        }
    }
}
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
int main(int argc,char **argv){
    REQUIRE(argc==2||argc==3);
    JSRuntime *rt=JS_NewRuntime();REQUIRE(rt);
    JSContext *ctx=JS_NewContext(rt);REQUIRE(ctx);
    JSValue global=JS_GetGlobalObject(ctx),pocket=JS_NewObject(ctx),kasane=JS_NewObject(ctx);
    REQUIRE(pocket_proc_install(ctx,kasane)==ESP_OK);
    REQUIRE(JS_SetPropertyStr(ctx,pocket,"kasane",kasane)>=0);
    REQUIRE(JS_SetPropertyStr(ctx,global,"pocket",pocket)>=0);
    JS_FreeValue(ctx,global);
    /* The view side is a permissive mock; megaBounds follows the image of
     * procedural surface 0, whose resource object is marked below. */
    eval_ok(ctx,
        "globalThis.megaBounds=[];globalThis.megaReplaces=0;"
        "globalThis.console={log(){}};"
        "const ref=()=>({setRect(){},setClip(){},setColor(){},setText(){},setReveal(){},"
        "setImageFrame(){},setRotation(){},place(){},setVisible(){},"
        "animate(){return {stop(){},finish(){},poll(){return 'running'}}}});"
        "const uiTx=new Proxy({},{get(o,k){return k==='image'?s=>{const r=ref(),main=s.resource.__s0;"
        "if(main)megaBounds=s.bounds.slice();"
        "r.setRect=(tx,b)=>{if(main)megaBounds=b.slice()};return r}:()=>ref()}});"
        "pocket.memory={info(){return {internalFreeBytes:1e9}}};"
        "pocket.kasane.replace=fn=>{megaReplaces++;fn(uiTx)};"
        "pocket.kasane.patch=fn=>fn(uiTx);"
        "pocket.kasane.stats=()=>({displayed:{commands:0}});"
        "pocket.kasane.resource=()=>({});"
        "pocket.kasane.cache={create(){return {}}};"
        "pocket.kasane.pixel={open(){return {}},stage(){return true}};"
        "pocket.kasane.grid={registerResizeSource(){return 1},"
        "resource(){return {__monitor:true}}}");
    ksn_image_port image;pocket_proc_image_port(&image);
    REQUIRE(image.width==KSN_PROC_W&&image.height==KSN_PROC_H);
    REQUIRE(image.variants==1&&image.frames==1);
    uint16_t span[KSN_PROC_W];uint8_t alpha[KSN_PROC_W];
    clock_t span_ticks=0;
    REQUIRE(image.read_span(image.ctx,1,0,0,0,1,span,alpha)==KSN_INVALID);
    REQUIRE(image.read_span(image.ctx,0,0,0,0,KSN_PROC_W,span,alpha)==KSN_OK);
    for(unsigned x=0;x<KSN_PROC_W;x++)REQUIRE(span[x]==0&&alpha[x]==255);
    eval_ok(ctx,"pocket.kasane.procedural.resource()");
    REQUIRE(pocket_proc_is_image_mode());
    size_t bytes;char *source=read_file(argv[1],&bytes);
    JSValue result=JS_Eval(ctx,source,bytes,argv[1],JS_EVAL_TYPE_GLOBAL);
    if(JS_IsException(result)){
        JSValue error=JS_GetException(ctx);
        const char *message=JS_ToCString(ctx,error);
        fprintf(stderr,"megademo JS eval: %s\n",message?message:"<non-string exception>");
        if(message)JS_FreeCString(ctx,message);
        JS_FreeValue(ctx,error);
    }
    REQUIRE(!JS_IsException(result));JS_FreeValue(ctx,result);free(source);
    for(unsigned tick=0;tick<PROC_MEGA_FRAMES;tick++){
        eval_ok(ctx,"frame()");
        REQUIRE(pocket_proc_pending());
        REQUIRE(pocket_proc_backdrop(NULL,0,KSN_PROC_H,actual)==KSN_OK);
        reference(tick);
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
        if(argc==3){
            char path[1024];
            REQUIRE(snprintf(path,sizeof path,"%s/frame-%02u.rgb565",argv[2],tick)>0);
            FILE *out=fopen(path,"wb");REQUIRE(out);
            for(unsigned i=0;i<KSN_PROC_W*KSN_PROC_H;i++){
                REQUIRE(fputc(expected[i]&255,out)!=EOF);
                REQUIRE(fputc(expected[i]>>8,out)!=EOF);
            }
            REQUIRE(fclose(out)==0);
        }
        if(tick<=1){
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
            REQUIRE(invalidates==0);
            pocket_proc_present_result(KSN_OK); /* UI ticket and image ACK */
            REQUIRE(!pocket_proc_pending());
            REQUIRE(image.read_span(image.ctx,0,0,25,7,31,span,alpha)==KSN_OK);
            REQUIRE(memcmp(span,expected+25*KSN_PROC_W+7,31*sizeof *span)==0);
            memcpy(previous,expected,sizeof previous);
        }else{
            pocket_proc_present_result(KSN_OK);
            REQUIRE(!pocket_proc_pending());
            REQUIRE(pocket_proc_backdrop(NULL,0,KSN_PROC_H,actual)==KSN_OK);
            REQUIRE(memcmp(actual,expected,sizeof actual)==0);
            REQUIRE(image.read_span(image.ctx,0,0,25,7,31,span,alpha)==KSN_OK);
            REQUIRE(memcmp(span,expected+25*KSN_PROC_W+7,31*sizeof *span)==0);
            memcpy(previous,expected,sizeof previous);
        }
    }
    REQUIRE(publishes==PROC_MEGA_FRAMES);
    eval_ok(ctx,"if(megaReplaces!==1||megaBounds.join(',')!=='0,0,240,135')"
                "throw Error('initial full-screen image or repeat replace')");
    uint32_t scalar=0,pie=0;
    pocket_proc_batch_counts(&scalar,&pie);
#ifdef KSN_PROC_HOST_FAKE_PIE
    REQUIRE(scalar==0&&pie==PROC_MEGA_FRAMES);
#else
    REQUIRE(scalar==PROC_MEGA_FRAMES&&pie==0);
#endif
    /* Two held Enter frames count as one press. The same resource reaches the
     * monitor and then returns to an exact full-screen rectangle. */
    for(unsigned i=0;i<44;i++){
        eval_ok(ctx,i<2?"frame(0x4000)":"frame(0)");
        REQUIRE(pocket_proc_pending());
        pocket_proc_present_result(KSN_OK);
    }
    /* Frame 48 enters TWIST, whose set is a new REPLACE; the zoom carries over. */
    eval_ok(ctx,"if(megaBounds.join(',')!=='64,20,176,83'||megaReplaces!==2)"
                "throw Error('Enter did not shrink into monitor')");
    for(unsigned i=0;i<44;i++){
        eval_ok(ctx,i==0?"frame(0x4000)":"frame(0)");
        REQUIRE(pocket_proc_pending());
        pocket_proc_present_result(KSN_OK);
    }
    eval_ok(ctx,"if(megaBounds.join(',')!=='0,0,240,135'||megaReplaces!==2)"
                "throw Error('Enter did not restore full screen')");
    eval_error(ctx,"pocket.kasane.procedural.register([[99,0,0,0,0,0]])");
    eval_ok(ctx,"pocket.kasane.procedural.beginFrame(0)");
    eval_error(ctx,"pocket.kasane.procedural.draw(1,[NaN,0,0,0])");
    pocket_proc_end_turn();
    eval_error(ctx,"pocket.kasane.procedural.commit()");
    pocket_proc_reset();
    REQUIRE(!pocket_proc_is_image_mode());
    REQUIRE(image.read_span(image.ctx,0,0,25,7,31,span,alpha)==KSN_OK);
    for(unsigned i=0;i<31;i++)REQUIRE(span[i]==0&&alpha[i]==255);
    eval_ok(ctx,"pocket.kasane.procedural.beginFrame(0)");
    eval_error(ctx,"pocket.kasane.procedural.draw(1,[0,0,0,0])");
    eval_ok(ctx,"let fresh=pocket.kasane.procedural.register([[0,0,0,0,1,0]]);"
                "pocket.kasane.procedural.beginFrame(63488);"
                "pocket.kasane.procedural.draw(fresh,[0,0,0,0]);"
                "pocket.kasane.procedural.commit()");
    pocket_proc_present_result(KSN_IO);
    REQUIRE(pocket_proc_has_frame());
    REQUIRE(invalidates>0); /* Backdrop mode repairs the old committed frame. */
    REQUIRE(pocket_proc_backdrop(NULL,0,KSN_PROC_H,actual)==KSN_OK);
    for(unsigned i=0;i<KSN_PROC_W*KSN_PROC_H;i++)REQUIRE(actual[i]==0);
    eval_error(ctx,"pocket.kasane.procedural.beginFrame(0)"); /* Old backdrop owes repair. */
    pocket_proc_overlay_present_result(KSN_OK); /* FLOWER cannot repair it. */
    eval_error(ctx,"pocket.kasane.procedural.beginFrame(0)");
    pocket_proc_present_result(KSN_OK);
    REQUIRE(!pocket_proc_has_frame());
    eval_ok(ctx,"pocket.kasane.procedural.beginFrame(0)");
    pocket_proc_end_turn();
    pocket_proc_reset();
    eval_ok(ctx,
        "const proc=pocket.kasane.procedural;let nestedRegisterBusy=false;"
        "let row=[0,0,0,0,1,0];"
        "Object.defineProperty(row,0,{get(){try{proc.register([[0,0,0,0,2,0]])}"
        "catch(e){nestedRegisterBusy=String(e).includes('BUSY')}return 0}});"
        "let guarded=proc.register([row]);"
        "if(!nestedRegisterBusy||!Number.isInteger(guarded))throw Error('register reentrancy');"
        "proc.beginFrame(7);let nestedCommitBusy=false;let inputs=[0,0,0,0];"
        "Object.defineProperty(inputs,0,{get(){try{proc.commit()}"
        "catch(e){nestedCommitBusy=String(e).includes('BUSY')}return 0}});"
        "proc.draw(guarded,inputs);"
        "if(!nestedCommitBusy)throw Error('draw reentrancy');proc.commit()");
    REQUIRE(pocket_proc_pending());
    REQUIRE(pocket_proc_backdrop(NULL,0,KSN_PROC_H,actual)==KSN_OK);
    for(unsigned i=0;i<KSN_PROC_W*KSN_PROC_H;i++)REQUIRE(actual[i]==7);
    pocket_proc_present_result(KSN_OK);
    pocket_proc_reset();
    eval_error(ctx,"pocket.kasane.procedural.register([[0,0,0,0,1,0]],"
                   "{kind:'affineQ14Points',x:[0,1],y:[0],coeff:[16384,0,0,16384,0,0],color:1})");
    eval_ok(ctx,"let off=pocket.kasane.procedural.register([[0,0,0,0,1,0]],"
                "{kind:'affineQ14Points',x:[0,1],y:[0,0],"
                /* x=721: one past the shared VM/typed bound of 720. */
                "coeff:[16384,0,0,16384,11812864,0],color:1});"
                "pocket.kasane.procedural.beginFrame(4)");
    eval_error(ctx,"pocket.kasane.procedural.draw(off,[0,0,0,0])");
    REQUIRE(!pocket_proc_pending());
    pocket_proc_reset();
    eval_ok(ctx,
        "let curve=proc.register(["
        "[0,0,0,0,10,0],[0,1,0,0,10,0],"
        "[0,2,0,0,35,0],[1,3,0,0,0,0],"
        "[0,4,0,0,75,0],[0,5,0,0,95,0],"
        "[0,6,0,0,100,0],[0,7,0,0,10,0],"
        "[14,0,16,0,0,2016]]);"
        "proc.beginFrame(0);proc.draw(curve,[95,0,0,0]);proc.commit()");
    REQUIRE(pocket_proc_pending());
    REQUIRE(pocket_proc_backdrop(NULL,0,KSN_PROC_H,actual)==KSN_OK);
    REQUIRE(actual[10*KSN_PROC_W+10]==0x07e0);
    REQUIRE(actual[10*KSN_PROC_W+100]==0x07e0);
    memcpy(previous,actual,sizeof previous);
    pocket_proc_present_result(KSN_OK);
    eval_ok(ctx,"proc.beginFrame(0);proc.draw(curve,[25,0,0,0]);proc.commit()");
    REQUIRE(pocket_proc_pending());
    REQUIRE(pocket_proc_backdrop(NULL,0,KSN_PROC_H,actual)==KSN_OK);
    REQUIRE(memcmp(previous,actual,sizeof actual)!=0);
    pocket_proc_present_result(KSN_OK);
    eval_error(ctx,"proc.register([[14,0,0,0,0,2016]])");
    pocket_proc_reset();
    /* Two image resources keep independent committed frames and reject a
     * second writer until the first candidate's display ticket is retired. */
    ksn_core_init(&multi_core);multi_active=true;
    eval_ok(ctx,
        "const layerId=proc.createSurface();"
        "if(!Number.isInteger(layerId)||layerId<=0)throw Error('surface ID');"
        "proc.beginFrame(0,layerId)");
    eval_error(ctx,"proc.commit()"); /* An extra surface needs its image lease. */
    pocket_proc_end_turn();
    eval_ok(ctx,
        "proc.resource();proc.resource(layerId);"
        "const dotPlan=proc.register([[1,0,0,0,0,0],[1,1,1,0,0,0],"
        "[8,0,0,1,0,63488]])");
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
    eval_ok(ctx,"proc.beginFrame(0);proc.draw(dotPlan,[10,10,0,0]);proc.commit()");
    eval_error(ctx,"proc.beginFrame(0,layerId)");
    multi_bytes=0;
    REQUIRE(ksn_render_rects(&multi_core,&multi_display,&multi_stats)==KSN_OK);
    REQUIRE(multi_bytes==64*48*2&&multi_panel[10*240+10]==0xf800);
    pocket_proc_overlay_present_result(KSN_OK);
    eval_ok(ctx,"proc.beginFrame(0,layerId);"
                "proc.draw(dotPlan,[20,10,0,0]);proc.commit()");
    REQUIRE(second.read_span(second.ctx,0,0,10,20,1,span,alpha)==KSN_OK);
    REQUIRE(span[0]==0xf800);
    REQUIRE(first.read_span(first.ctx,0,0,10,10,1,span,alpha)==KSN_OK);
    REQUIRE(span[0]==0xf800);
    multi_bytes=0;
    REQUIRE(ksn_render_rects(&multi_core,&multi_display,&multi_stats)==KSN_OK);
    REQUIRE(multi_bytes==64*48*2&&multi_panel[10*240+100]==0xf800);
    pocket_proc_overlay_present_result(KSN_OK);
    REQUIRE(!pocket_proc_pending());
    eval_ok(ctx,"proc.beginFrame(0);proc.draw(dotPlan,[11,10,0,0]);proc.commit()");
    REQUIRE(second.read_span(second.ctx,0,0,10,20,1,span,alpha)==KSN_OK&&span[0]==0xf800);
    multi_bytes=0;
    REQUIRE(ksn_render_rects(&multi_core,&multi_display,&multi_stats)==KSN_OK);
    REQUIRE(multi_bytes==16*8*2&&multi_panel[10*240+10]==0&&
            multi_panel[10*240+11]==0xf800&&multi_panel[10*240+100]==0xf800);
    pocket_proc_overlay_present_result(KSN_OK);
    eval_ok(ctx,"proc.beginFrame(0,layerId);proc.draw(dotPlan,[21,10,0,0]);proc.commit()");
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
    eval_error(ctx,"proc.createSurface()");
    REQUIRE(ksn_core_reset_layer(&multi_core,KSN_APP)==KSN_OK);
    ksn_image_port stale_port;
    REQUIRE(ksn_core_image_port(&multi_core,KSN_APP,multi_resource[0],&stale_port)==KSN_STALE);
    REQUIRE(ksn_core_image_port(&multi_core,KSN_APP,multi_resource[1],&stale_port)==KSN_STALE);
    multi_active=false;
    pocket_proc_reset();
    eval_error(ctx,"proc.resource(layerId)");
    eval_ok(ctx,"const newLayerId=proc.createSurface();"
                "if(newLayerId===layerId)throw Error('stale surface ID reused')");
    pocket_proc_reset();
    /* Same plan and raster path: reusable typed single and bulk agree with Array. */
    eval_ok(ctx,"const typedPlan=proc.register([[1,0,0,0,0,0],[1,1,1,0,0,0],[8,0,0,1,0,63488]]);"
                "const typedInputs=new Float64Array([12,13,0,0,0,0,0,0]);"
                "proc.beginFrame(0);proc.draw(typedPlan,[12,13]);proc.commit()");
    uint16_t typed_reference[KSN_PROC_W*KSN_PROC_H],typed_actual[KSN_PROC_W*KSN_PROC_H];
    REQUIRE(pocket_proc_backdrop(NULL,0,KSN_PROC_H,typed_reference)==KSN_OK);
    pocket_proc_present_result(KSN_OK);
    for(unsigned repetition=0;repetition<16;repetition++){
        eval_ok(ctx,"proc.beginFrame(0);proc.draw(typedPlan,typedInputs);proc.commit()");
        REQUIRE(pocket_proc_backdrop(NULL,0,KSN_PROC_H,typed_actual)==KSN_OK);
        REQUIRE(memcmp(typed_reference,typed_actual,sizeof typed_actual)==0);
        pocket_proc_present_result(KSN_OK);
        eval_ok(ctx,"proc.beginFrame(0);proc.drawBulk(new Int32Array([typedPlan]),typedInputs);proc.commit()");
        REQUIRE(pocket_proc_backdrop(NULL,0,KSN_PROC_H,typed_actual)==KSN_OK);
        REQUIRE(memcmp(typed_reference,typed_actual,sizeof typed_actual)==0);
        pocket_proc_present_result(KSN_OK);
    }
    eval_ok(ctx,"proc.beginFrame(0);proc.drawBulk(new Int32Array([typedPlan,typedPlan]),"
                "new Float64Array([12,13,0,0,0,0,0,0,12,13,0,0,0,0,0,0]));proc.commit()");
    REQUIRE(pocket_proc_backdrop(NULL,0,KSN_PROC_H,typed_actual)==KSN_OK);
    REQUIRE(memcmp(typed_reference,typed_actual,sizeof typed_actual)==0);
    pocket_proc_present_result(KSN_OK);
    eval_ok(ctx,"proc.beginFrame(0)");
    eval_error(ctx,"proc.drawBulk(new Int32Array([typedPlan]),new Float64Array(7))");
    eval_error(ctx,"proc.drawBulk(new Int32Array([typedPlan,2147483647]),new Float64Array(16))");
    eval_error(ctx,"proc.draw(typedPlan,new Float32Array([12,13]))");
    eval_error(ctx,"proc.draw(typedPlan,new Float64Array(new SharedArrayBuffer(64)))");
    eval_ok(ctx,"const detachedInput=new Float64Array(8);detachedInput.buffer.transfer()");
    eval_error(ctx,"proc.draw(typedPlan,detachedInput)");
    eval_error(ctx,"proc.draw(typedPlan,new Float64Array([NaN]))");
    eval_error(ctx,"proc.draw(typedPlan,new Float64Array([Infinity]))");
    eval_error(ctx,"proc.draw(typedPlan,new Float64Array([1e300]))");
    eval_error(ctx,"proc.drawBulk(new Int32Array(9),new Float64Array(72))");
    eval_ok(ctx,"proc.draw(typedPlan,typedInputs);proc.commit()");
    pocket_proc_present_result(KSN_OK);
    eval_ok(ctx,"proc.beginFrame(0);for(let i=0;i<1023;i++)proc.draw(typedPlan,typedInputs)");
    eval_error(ctx,"proc.drawBulk(new Int32Array([typedPlan,typedPlan]),new Float64Array(16))");
    REQUIRE(!pocket_proc_pending());
    eval_error(ctx,"proc.commit()");
    eval_ok(ctx,"proc.beginFrame(0);proc.draw(typedPlan,typedInputs);proc.commit()");
    REQUIRE(pocket_proc_backdrop(NULL,0,KSN_PROC_H,typed_actual)==KSN_OK);
    REQUIRE(memcmp(typed_reference,typed_actual,sizeof typed_actual)==0);
    pocket_proc_present_result(KSN_OK);
    /* Offset view keeps the same binary64 values without requiring alignment assumptions. */
    eval_ok(ctx,"const offsetInputs=new Float64Array(new ArrayBuffer(80),8,8);"
                "offsetInputs.set(typedInputs);proc.beginFrame(0);proc.draw(typedPlan,offsetInputs);proc.commit()");
    REQUIRE(pocket_proc_backdrop(NULL,0,KSN_PROC_H,typed_actual)==KSN_OK);
    REQUIRE(memcmp(typed_reference,typed_actual,sizeof typed_actual)==0);
    pocket_proc_present_result(KSN_OK);
    eval_ok(ctx,"proc.beginFrame(0);proc.draw(typedPlan,[]);proc.commit()");
    REQUIRE(pocket_proc_backdrop(NULL,0,KSN_PROC_H,typed_reference)==KSN_OK);
    pocket_proc_present_result(KSN_OK);
    eval_ok(ctx,"proc.beginFrame(0);proc.draw(typedPlan,new Float64Array(0));proc.commit()");
    REQUIRE(pocket_proc_backdrop(NULL,0,KSN_PROC_H,typed_actual)==KSN_OK);
    REQUIRE(memcmp(typed_reference,typed_actual,sizeof typed_actual)==0);
    pocket_proc_present_result(KSN_OK);
    eval_ok(ctx,"proc.beginFrame(0);proc.draw(typedPlan,new Float64Array([-0,5e-324]));proc.commit()");
    REQUIRE(pocket_proc_backdrop(NULL,0,KSN_PROC_H,typed_actual)==KSN_OK);
    REQUIRE(memcmp(typed_reference,typed_actual,sizeof typed_actual)==0);
    pocket_proc_present_result(KSN_OK);
    /* Unused inputs still undergo the public float conversion and validation. */
    eval_ok(ctx,"const noopTyped=proc.register([[0,0,0,0,0,0]]);proc.beginFrame(0);"
                "proc.draw(noopTyped,[3.4028234663852886e38,-3.4028234663852886e38]);"
                "proc.draw(noopTyped,new Float64Array([3.4028234663852886e38,-3.4028234663852886e38]));"
                "proc.draw(noopTyped,new Float64Array([3.4028235e38,-3.4028235e38]));"
                "proc.draw(noopTyped,new Float64Array([0,-0,5e-324,-5e-324,1e-40,-1e-40]));proc.commit()");
    pocket_proc_present_result(KSN_OK);
    eval_ok(ctx,"proc.beginFrame(0)");
    eval_error(ctx,"proc.draw(noopTyped,new Float64Array([3.4028236e38]))");
    eval_error(ctx,"proc.draw(noopTyped,new Float64Array([-3.4028236e38]))");
    pocket_proc_end_turn();
    /* Overlapping different colors make both row order and row omission visible. */
    eval_ok(ctx,"const orderedPlans=Array.from({length:8},(_,i)=>proc.register("
                "[[1,0,0,0,0,0],[1,1,1,0,0,0],[8,0,0,1,0,4097+i],"
                "[0,2,0,0,1+i,0],[0,3,0,0,1,0],[8,0,2,3,0,4097+i]]));"
                "const orderedHandles=new Int32Array(orderedPlans),orderedInputs=new Float64Array(64);"
                "for(let i=0;i<8;i++){orderedInputs[i*8]=12;orderedInputs[i*8+1]=13;}"
                "proc.beginFrame(0);for(let i=0;i<8;i++)proc.draw(orderedPlans[i],[12,13]);proc.commit()");
    REQUIRE(pocket_proc_backdrop(NULL,0,KSN_PROC_H,typed_reference)==KSN_OK);
    REQUIRE(typed_reference[13*KSN_PROC_W+12]==4104);
    for(unsigned i=0;i<8;i++)REQUIRE(typed_reference[KSN_PROC_W+1+i]==4097+i);
    pocket_proc_present_result(KSN_OK);
    eval_ok(ctx,"proc.beginFrame(0);proc.drawBulk(orderedHandles,orderedInputs);proc.commit()");
    REQUIRE(pocket_proc_backdrop(NULL,0,KSN_PROC_H,typed_actual)==KSN_OK);
    REQUIRE(memcmp(typed_reference,typed_actual,sizeof typed_actual)==0);
    pocket_proc_present_result(KSN_OK);
    /* Invalid later rows must not append the valid first row. The current frame
     * contains a different pixel, so partial submission cannot hide in overlap. */
    eval_ok(ctx,"proc.beginFrame(0);proc.draw(typedPlan,[20,21])");
    eval_error(ctx,"proc.drawBulk(new Int32Array([orderedPlans[0],2147483647]),orderedInputs.subarray(0,16))");
    eval_error(ctx,"proc.drawBulk(orderedHandles,(()=>{const p=orderedInputs.slice();p[63]=NaN;return p})())");
    eval_ok(ctx,"proc.draw(typedPlan,[22,23]);proc.commit()");
    REQUIRE(pocket_proc_backdrop(NULL,0,KSN_PROC_H,typed_actual)==KSN_OK);
    REQUIRE(typed_actual[13*KSN_PROC_W+12]==0);
    REQUIRE(typed_actual[21*KSN_PROC_W+20]==0xf800);
    REQUIRE(typed_actual[23*KSN_PROC_W+22]==0xf800);
    pocket_proc_present_result(KSN_OK);
    pocket_proc_reset();
    JS_FreeContext(ctx);JS_FreeRuntime(rt);
    printf("PASS real QuickJS procedural adapter: 48 exact RGB565 frames, two independent image surfaces, 256 B rectangle damage, IO repair, stale resources, Enter toggle; image span CPU %.1f ms\n",
           1000.0*(double)span_ticks/CLOCKS_PER_SEC);
    return 0;
}
