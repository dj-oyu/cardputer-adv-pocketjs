#include "quickjs.h"
#include "pocket_pixel.h"
#include "pocket_api.h"
#include "core_fixture.h"
#include "ksn_render.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do {if(!(x)){fprintf(stderr,"pixel JS test %s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}}while(0)
KSN_TEST_CORE(core,static);
static ksn_image_port image;
static ksn_resource resource;
static bool can_stage=true;
static unsigned invalidates,sends,fail_on_send;
static uint16_t panel[240*135],strip[240*8];
JSValue pocket_kasane_pixel_resource(JSContext *ctx,const ksn_image_port *port){
    CHECK(port->width==8&&port->height==8&&port->frames==1);
    image=*port;
    if(!resource.value)CHECK(ksn_core_register_image(&core,KSN_APP,port,&resource)==KSN_OK);
    return JS_NewObject(ctx);
}
bool pocket_kasane_pixel_can_stage(void){return can_stage;}
void pocket_kasane_pixel_invalidate(void){
    ++invalidates;(void)ksn_core_invalidate_image(&core,resource);
}
JSValue pocket_api_throw(JSContext *ctx,const char *code,const char *op,
                         const char *message,bool retryable,const char *outcome){
    (void)retryable;(void)outcome;
    return JS_ThrowTypeError(ctx,"%s %s: %s",code,op,message);
}
static void eval(JSContext *ctx,const char *script,bool error){
    JSValue result=JS_Eval(ctx,script,strlen(script),"pixel.js",JS_EVAL_TYPE_GLOBAL);
    if(JS_IsException(result)&&!error){
        JSValue e=JS_GetException(ctx);const char *s=JS_ToCString(ctx,e);
        fprintf(stderr,"unexpected JS exception: %s\n",s?s:"unknown");
        if(s)JS_FreeCString(ctx,s);JS_FreeValue(ctx,e);
    }
    CHECK(JS_IsException(result)==error);
    if(error){JSValue e=JS_GetException(ctx);JS_FreeValue(ctx,e);}
    JS_FreeValue(ctx,result);
}
static uint16_t *buffer(void *ctx){(void)ctx;return strip;}
static ksn_result send(void *ctx,uint16_t y,uint16_t rows,const uint16_t *pixels){
    (void)ctx;if(++sends==fail_on_send)return KSN_IO;
    memcpy(panel+(size_t)y*240,pixels,(size_t)rows*240*2);return KSN_OK;
}
static void expect_pixel(uint16_t color){
    uint16_t out[2];uint8_t alpha[2];
    CHECK(image.read_span(image.ctx,0,0,1,1,2,out,alpha)==KSN_OK);
    CHECK(out[0]==color&&out[1]==color&&alpha[0]==255&&alpha[1]==255);
}
int main(void){
    ksn_core_init(&core);
    JSRuntime *rt=JS_NewRuntime();CHECK(rt);
    JSContext *ctx=JS_NewContext(rt);CHECK(ctx);
    JSValue kasane=JS_NewObject(ctx);CHECK(pocket_pixel_install(ctx,kasane)==ESP_OK);
    JSValue global=JS_GetGlobalObject(ctx);
    CHECK(JS_SetPropertyStr(ctx,global,"kasane",kasane)>=0);JS_FreeValue(ctx,global);
    eval(ctx,"kasane.pixel.open(113,8)",true);
    eval(ctx,"kasane.pixel.stage(new Uint16Array([1,0,0,0,1]),new Uint16Array(8),0,0)",true);
    eval(ctx,"globalThis.resource=kasane.pixel.open(8,8);"
             "globalThis.code=new Uint16Array([1,0,0,0,4660,1,1,0,0,255]);"
             "globalThis.params=new Uint16Array(8);"
             "if(!kasane.pixel.stage(code,params,0,1))throw Error('stage')",false);
    CHECK(pocket_pixel_pending()&&invalidates==1);expect_pixel(4660);
    eval(ctx,"if(kasane.pixel.stage(code,params,0,1))throw Error('pending accepted')",false);
    ksn_client app=ksn_core_client(&core,KSN_APP);
    ksn_draw draw={.kind=KSN_IMAGE,.bounds={10,3,18,11},
        .clip={0,0,240,135},.opacity=255,
        .data.image={.resource=resource,.scale=KSN_IMAGE_1X}};
    ksn_tx tx;ksn_ref first,second;
    CHECK(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    CHECK(app.ops->background(app.ctx,tx,0x000000ff)==KSN_OK);
    CHECK(app.ops->add(app.ctx,tx,&draw,&first)==KSN_OK);
    draw.bounds.x0=30;draw.bounds.x1=38;
    CHECK(app.ops->add(app.ctx,tx,&draw,&second)==KSN_OK);
    CHECK(app.ops->end(app.ctx,tx)==KSN_OK);
    ksn_display_port display={NULL,buffer,send,240,135,8,NULL,NULL};
    ksn_render_stats stats;
    sends=0;fail_on_send=2;
    CHECK(ksn_render_rects(&core,&display,&stats)==KSN_IO);
    pocket_pixel_present_result(KSN_IO);CHECK(pocket_pixel_pending());expect_pixel(4660);
    sends=0;fail_on_send=0;
    CHECK(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    CHECK(panel[4*240+11]==4660&&panel[4*240+31]==4660);
    pocket_pixel_present_result(KSN_OK);CHECK(!pocket_pixel_pending());
    can_stage=false;
    eval(ctx,"code[4]=22136;if(kasane.pixel.stage(code,params,0,1))throw Error('busy accepted')",false);
    can_stage=true;
    eval(ctx,"if(!kasane.pixel.stage(code,params,0,1))throw Error('stage2')",false);
    expect_pixel(22136);
    CHECK(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    CHECK(panel[4*240+11]==22136&&panel[4*240+31]==22136);
    pocket_pixel_present_result(KSN_OK);CHECK(!pocket_pixel_pending());
    eval(ctx,"code[0]=5;kasane.pixel.stage(code,params,0,1)",true);
    eval(ctx,"kasane.pixel.open(4,4)",true);
    pocket_pixel_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);
    puts("pocket pixel QuickJS: PASS");return 0;
}
