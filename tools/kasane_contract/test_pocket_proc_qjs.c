#include "quickjs.h"
#include "pocket_proc.h"
#include "pocket_api.h"
#include "proc_megademo.h"
#include "ksn_proc_plan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(x) do { if(!(x)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);} } while(0)
static uint16_t actual[KSN_PROC_W*KSN_PROC_H], expected[KSN_PROC_W*KSN_PROC_H];
static uint16_t previous[KSN_PROC_W*KSN_PROC_H];
static unsigned publishes,invalidates;
#ifdef KSN_PROC_HOST_FAKE_PIE
void ksn_proc_points_affine_pie(KsnProcPointDst dst,KsnProcPointSrc src,
                                size_t n,const KsnProcAffineQ14 *coeff){
    ksn_proc_points_affine_scalar(dst,src,n,coeff);
}
#endif

ksn_result pocket_kasane_proc_publish(void){publishes++;return KSN_OK;}
void pocket_kasane_invalidate(void){invalidates++;}
JSValue pocket_kasane_proc_resource(JSContext *ctx){
    pocket_proc_image_mode();
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
int main(int argc,char **argv){
    REQUIRE(argc==2||argc==3);
    JSRuntime *rt=JS_NewRuntime();REQUIRE(rt);
    JSContext *ctx=JS_NewContext(rt);REQUIRE(ctx);
    JSValue global=JS_GetGlobalObject(ctx),pocket=JS_NewObject(ctx),kasane=JS_NewObject(ctx);
    REQUIRE(pocket_proc_install(ctx,kasane)==ESP_OK);
    REQUIRE(JS_SetPropertyStr(ctx,pocket,"kasane",kasane)>=0);
    REQUIRE(JS_SetPropertyStr(ctx,global,"pocket",pocket)>=0);
    JS_FreeValue(ctx,global);
    ksn_image_port image;pocket_proc_image_port(&image);
    REQUIRE(image.width==KSN_PROC_W&&image.height==KSN_PROC_H);
    REQUIRE(image.variants==1&&image.frames==1);
    uint16_t span[KSN_PROC_W];uint8_t alpha[KSN_PROC_W];
    REQUIRE(image.read_span(image.ctx,1,0,0,0,1,span,alpha)==KSN_INVALID);
    REQUIRE(image.read_span(image.ctx,0,0,0,0,KSN_PROC_W,span,alpha)==KSN_OK);
    for(unsigned x=0;x<KSN_PROC_W;x++)REQUIRE(span[x]==0&&alpha[x]==255);
    eval_ok(ctx,"pocket.kasane.procedural.resource()");
    REQUIRE(pocket_proc_is_image_mode());
    size_t bytes;char *source=read_file(argv[1],&bytes);
    JSValue result=JS_Eval(ctx,source,bytes,argv[1],JS_EVAL_TYPE_GLOBAL);
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
            pocket_proc_present_result(KSN_IO); /* repeated transfer failure */
            REQUIRE(pocket_proc_pending());
            REQUIRE(image.read_span(image.ctx,0,0,25,7,31,span,alpha)==KSN_OK);
            REQUIRE(memcmp(span,expected+25*KSN_PROC_W+7,31*sizeof *span)==0);
            REQUIRE(invalidates>0);
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
    uint32_t scalar=0,pie=0;
    pocket_proc_batch_counts(&scalar,&pie);
#ifdef KSN_PROC_HOST_FAKE_PIE
    REQUIRE(scalar==0&&pie==PROC_MEGA_FRAMES);
#else
    REQUIRE(scalar==PROC_MEGA_FRAMES&&pie==0);
#endif
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
    REQUIRE(pocket_proc_backdrop(NULL,0,KSN_PROC_H,actual)==KSN_OK);
    for(unsigned i=0;i<KSN_PROC_W*KSN_PROC_H;i++)REQUIRE(actual[i]==0);
    pocket_proc_present_result(KSN_OK);
    REQUIRE(!pocket_proc_has_frame());
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
                "coeff:[16384,0,0,16384,4915200,0],color:1});"
                "pocket.kasane.procedural.beginFrame(4)");
    eval_error(ctx,"pocket.kasane.procedural.draw(off,[0,0,0,0])");
    REQUIRE(!pocket_proc_pending());
    pocket_proc_reset();
    JS_FreeContext(ctx);JS_FreeRuntime(rt);
    puts("PASS real QuickJS procedural adapter: 48 candidate frames with typed points, exact RGB565, rollback, repair, malformed input, reset and reentrancy");
    return 0;
}
