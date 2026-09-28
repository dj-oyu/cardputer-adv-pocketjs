/* Host reference for the limits scene of apps/kasane/proc_limits_probe.js.
 *
 * The scene (8 inputs, r8..r15, REPEAT depth 8, a 128-point and an 8-point
 * typed batch at the -480..720 bounds, an off-panel VM line) is read from the
 * same JS file the device runs. The reference is built without the adapter:
 * every VM draw runs one ksn_proc_step() at a time and typed points use the
 * scalar kernel. The real pocket_proc.c adapter then draws the same scene on
 * host and must match it; the device capture is compared against the file
 * this writes (tools/kasane_contract/run_proc_limits_device.py).
 *
 * usage: test_proc_limits_scene <proc_limits_probe.js> [expected.rgb565] */
#include "quickjs.h"
#include "pocket_proc.h"
#include "pocket_api.h"
#include "ksn_proc_plan.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(x) do { if(!(x)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);} } while(0)
static uint16_t expected[KSN_PROC_W*KSN_PROC_H],actual[KSN_PROC_W*KSN_PROC_H];
static uint16_t banded[KSN_PROC_W*KSN_PROC_H];
static ksn_proc_frame reference,scratch;
static ksn_proc_vm vm;

ksn_result pocket_kasane_proc_publish(void){return KSN_OK;}
ksn_result pocket_kasane_proc_publish_at(unsigned surface,ksn_rect damage){
    (void)surface;(void)damage;return KSN_OK;
}
void pocket_kasane_invalidate(void){}
JSValue pocket_kasane_proc_resource(JSContext *ctx){pocket_proc_image_mode();return JS_NewObject(ctx);}
JSValue pocket_kasane_proc_resource_at(JSContext *ctx,unsigned surface){
    pocket_proc_image_mode_at(surface);return JS_NewObject(ctx);
}
JSValue pocket_api_throw(JSContext *ctx,const char *code,const char *op,
                         const char *message,bool retryable,const char *outcome){
    (void)retryable;(void)outcome;
    return JS_ThrowTypeError(ctx,"%s %s: %s",code,op,message);
}
static void check(JSContext *ctx,JSValue v){
    if(JS_IsException(v)){
        JSValue e=JS_GetException(ctx);const char *m=JS_ToCString(ctx,e);
        fprintf(stderr,"QuickJS: %s\n",m?m:"exception");exit(1);
    }
    JS_FreeValue(ctx,v);
}
static double num(JSContext *ctx,JSValueConst o,uint32_t i){
    JSValue v=JS_GetPropertyUint32(ctx,o,i);double d;
    REQUIRE(JS_ToFloat64(ctx,&d,v)==0&&isfinite(d));JS_FreeValue(ctx,v);return d;
}
static uint32_t len(JSContext *ctx,JSValueConst o){
    JSValue v=JS_GetPropertyStr(ctx,o,"length");uint32_t n;
    REQUIRE(JS_ToUint32(ctx,&n,v)==0);JS_FreeValue(ctx,v);return n;
}
static void append(const ksn_proc_frame *src){
    REQUIRE(reference.count+src->count<=KSN_PROC_SEGMENTS);
    memcpy(&reference.segments[reference.count],src->segments,src->count*sizeof src->segments[0]);
    reference.count+=src->count;
}
/* One scene entry: VM segments first, then the typed polyline, as draw() does. */
static void draw_reference(JSContext *ctx,JSValueConst d,unsigned *steps){
    JSValue code=JS_GetPropertyStr(ctx,d,"code");
    uint32_t n=len(ctx,code);REQUIRE(n>=1&&n<=KSN_PROC_CODE);
    ksn_proc_inst inst[KSN_PROC_CODE];
    for(uint32_t i=0;i<n;i++){
        JSValue row=JS_GetPropertyUint32(ctx,code,i);
        inst[i]=(ksn_proc_inst){(uint8_t)num(ctx,row,0),(uint8_t)num(ctx,row,1),
            (uint8_t)num(ctx,row,2),(uint8_t)num(ctx,row,3),(float)num(ctx,row,4),
            (uint16_t)num(ctx,row,5)};
        JS_FreeValue(ctx,row);
    }
    JS_FreeValue(ctx,code);
    float in[KSN_PROC_INPUTS]={0};
    JSValue inputs=JS_GetPropertyStr(ctx,d,"inputs");
    uint32_t ni=len(ctx,inputs);REQUIRE(ni<=KSN_PROC_INPUTS);
    for(uint32_t i=0;i<ni;i++)in[i]=(float)num(ctx,inputs,i);
    JS_FreeValue(ctx,inputs);
    const ksn_proc_program p={inst,(uint8_t)n};
    ksn_proc_status s=ksn_proc_begin(&vm,&p,in,&scratch);
    while(s==KSN_PROC_RUNNING)s=ksn_proc_step(&vm);
    REQUIRE(s==KSN_PROC_DONE);
    *steps+=vm.steps;
    append(&scratch);
    JSValue pts=JS_GetPropertyStr(ctx,d,"points");
    if(JS_IsObject(pts)){
        JSValue x=JS_GetPropertyStr(ctx,pts,"x"),y=JS_GetPropertyStr(ctx,pts,"y");
        JSValue c=JS_GetPropertyStr(ctx,pts,"coeff"),col=JS_GetPropertyStr(ctx,pts,"color");
        uint32_t np=len(ctx,x);REQUIRE(np>=2&&np<=128&&len(ctx,y)==np);
        static _Alignas(16) int16_t sx[128],sy[128],ox[128],oy[128];
        for(uint32_t i=0;i<np;i++){sx[i]=(int16_t)num(ctx,x,i);sy[i]=(int16_t)num(ctx,y,i);}
        const KsnProcAffineQ14 k={(int16_t)num(ctx,c,0),(int16_t)num(ctx,c,1),
            (int16_t)num(ctx,c,2),(int16_t)num(ctx,c,3),(int32_t)num(ctx,c,4),(int32_t)num(ctx,c,5)};
        double color;REQUIRE(JS_ToFloat64(ctx,&color,col)==0);
        ksn_proc_points_affine_scalar((KsnProcPointDst){ox,oy},(KsnProcPointSrc){sx,sy},np,&k);
        memset(&scratch,0,sizeof scratch);
        for(uint32_t i=1;i<np;i++){
            REQUIRE(ox[i]>=-480&&ox[i]<=720&&oy[i]>=-480&&oy[i]<=720);
            scratch.segments[scratch.count++]=(ksn_proc_segment){ox[i-1],oy[i-1],ox[i],oy[i],(uint16_t)color};
        }
        append(&scratch);
        JS_FreeValue(ctx,x);JS_FreeValue(ctx,y);JS_FreeValue(ctx,c);JS_FreeValue(ctx,col);
    }
    JS_FreeValue(ctx,pts);
}
static uint32_t fnv(const uint16_t *p){
    uint32_t h=2166136261u;
    for(unsigned i=0;i<KSN_PROC_W*KSN_PROC_H;i++){
        h=(h^(p[i]&255u))*16777619u;h=(h^(p[i]>>8))*16777619u;
    }
    return h;
}
int main(int argc,char **argv){
    REQUIRE(argc==2||argc==3);
    FILE *f=fopen(argv[1],"rb");REQUIRE(f);
    fseek(f,0,SEEK_END);long n=ftell(f);fseek(f,0,SEEK_SET);
    char *src=malloc((size_t)n+1);REQUIRE(src&&fread(src,1,(size_t)n,f)==(size_t)n);
    fclose(f);src[n]=0;
    JSRuntime *rt=JS_NewRuntime();JSContext *ctx=JS_NewContext(rt);
    check(ctx,JS_Eval(ctx,src,(size_t)n,argv[1],JS_EVAL_TYPE_GLOBAL));
    JSValue g=JS_GetGlobalObject(ctx);
    JSValue scene=JS_GetPropertyStr(ctx,g,"limScene");REQUIRE(JS_IsObject(scene));
    JSValue bgv=JS_GetPropertyStr(ctx,scene,"bg");double bg;REQUIRE(JS_ToFloat64(ctx,&bg,bgv)==0);
    JSValue draws=JS_GetPropertyStr(ctx,scene,"draws");
    uint32_t nd=len(ctx,draws);unsigned steps=0;
    reference.ready=true;
    for(uint32_t i=0;i<nd;i++){
        JSValue d=JS_GetPropertyUint32(ctx,draws,i);draw_reference(ctx,d,&steps);JS_FreeValue(ctx,d);
    }
    for(unsigned i=0;i<KSN_PROC_W*KSN_PROC_H;i++)expected[i]=banded[i]=(uint16_t)bg;
    REQUIRE(ksn_proc_render_band(&reference,expected,0,KSN_PROC_H));
    for(int y=0;y<KSN_PROC_H;y+=8){
        int rows=KSN_PROC_H-y<8?KSN_PROC_H-y:8;
        REQUIRE(ksn_proc_render_band(&reference,banded+y*KSN_PROC_W,y,rows));
    }
    REQUIRE(!memcmp(expected,banded,sizeof expected));
    /* The adapter path, same scene, same JS values. */
    JSValue ns=JS_NewObject(ctx);
    REQUIRE(pocket_proc_install(ctx,ns)==ESP_OK);
    REQUIRE(JS_SetPropertyStr(ctx,g,"kas",ns)>=0);
    static const char run[]=
        "const P=kas.procedural,S=limScene;"
        "const hs=S.draws.map(d=>d.points?P.register(d.code,d.points):P.register(d.code));"
        "P.beginFrame(S.bg);S.draws.forEach((d,i)=>P.draw(hs[i],d.inputs));P.commit();";
    check(ctx,JS_Eval(ctx,run,sizeof run-1,"adapter",JS_EVAL_TYPE_GLOBAL));
    REQUIRE(pocket_proc_pending());
    for(int y=0;y<KSN_PROC_H;y+=8){
        int rows=KSN_PROC_H-y<8?KSN_PROC_H-y:8;
        REQUIRE(pocket_proc_backdrop(NULL,(uint16_t)y,(uint16_t)rows,actual+y*KSN_PROC_W)==KSN_OK);
    }
    REQUIRE(!memcmp(actual,expected,sizeof expected));
    unsigned lit=0;
    for(unsigned i=0;i<KSN_PROC_W*KSN_PROC_H;i++)lit+=expected[i]!=(uint16_t)bg;
    printf("limits scene: draws=%u segments=%u vm_steps=%u lit=%u fnv=%u adapter=match\n",
           nd,(unsigned)reference.count,steps,lit,(unsigned)fnv(expected));
    if(argc==3){
        FILE *o=fopen(argv[2],"wb");REQUIRE(o);
        for(unsigned i=0;i<KSN_PROC_W*KSN_PROC_H;i++){fputc(expected[i]&255,o);fputc(expected[i]>>8,o);}
        REQUIRE(fclose(o)==0);
    }
    pocket_proc_reset();
    JS_FreeValue(ctx,draws);JS_FreeValue(ctx,bgv);JS_FreeValue(ctx,scene);JS_FreeValue(ctx,g);
    JS_FreeContext(ctx);JS_FreeRuntime(rt);free(src);
    return 0;
}
