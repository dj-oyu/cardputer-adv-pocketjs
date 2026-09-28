#include "pocket_pixel.h"
#include "pocket_api.h"
#include "pocket_kasane.h"
#include "ui/kasane/ksn_pixel_function.h"
#include "ui/kasane/ksn_pixel_span.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* One session, one logical image frame. Every retained node reads the same
 * selected slot, so a successful present retires all references to the old
 * slot together. No underlay or independent frame leases are exposed. */
typedef struct { ksn_pixel_pool *pool; bool open; } pixel_state;
static pixel_state pixel;

static JSValue fail(JSContext *ctx,const char *op,const char *code,const char *message){
    return pocket_api_throw(ctx,code,op,message,false,POCKET_OUTCOME_NOT_APPLIED);
}
static bool integer(JSContext *ctx,JSValueConst value,unsigned max,unsigned *out){
    double number;
    if(!JS_IsNumber(value)||JS_ToFloat64(ctx,&number,value)<0||
       !isfinite(number)||number<0||number>max||floor(number)!=number)return false;
    *out=(unsigned)number;return true;
}
static bool words(JSContext *ctx,JSValueConst value,size_t expected,
                  const uint16_t **data,JSValue *buffer){
    if(JS_GetTypedArrayType(value)!=JS_TYPED_ARRAY_UINT16)return false;
    size_t offset=0,bytes=0,element=0;
    *buffer=JS_GetTypedArrayBuffer(ctx,value,&offset,&bytes,&element);
    if(JS_IsException(*buffer))return false;
    size_t size=0;uint8_t *raw=JS_GetArrayBuffer(ctx,&size,*buffer);
    if(!raw||element!=2||bytes!=expected*2||offset>size||bytes>size-offset){
        JS_FreeValue(ctx,*buffer);*buffer=JS_UNDEFINED;return false;
    }
    *data=(const uint16_t *)(raw+offset);return true;
}
static ksn_result read_span(void *ctx,uint16_t variant,uint16_t frame,
                            uint16_t y,uint16_t x,uint16_t count,
                            uint16_t *rgb565,uint8_t *alpha){
    pixel_state *p=ctx;
    if(!p||!p->open||variant||frame)return KSN_INVALID;
    ksn_pixel_handle selected=p->pool->pending.generation?
        p->pool->pending:p->pool->committed;
    if(!selected.generation){
        if(y>=p->pool->height||x>p->pool->width||count>p->pool->width-x||
           (count&&(!rgb565||!alpha)))return KSN_INVALID;
        if(count){memset(rgb565,0,(size_t)count*2);memset(alpha,0,count);}
        return KSN_OK;
    }
    return ksn_pixel_span_eval(&p->pool->slots[selected.slot].image,
                               y,x,count,rgb565,alpha)?KSN_OK:KSN_INVALID;
}
void pocket_pixel_reset(void){free(pixel.pool);memset(&pixel,0,sizeof pixel);}
bool pocket_pixel_pending(void){return pixel.open&&pixel.pool->pending.generation;}
void pocket_pixel_present_result(ksn_result result){
    if(pocket_pixel_pending()&&result==KSN_OK)
        (void)ksn_pixel_pool_finish(pixel.pool,pixel.pool->pending,true,true);
}
static JSValue open_pixel(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;const char *op="kasane.pixel.open";
    unsigned width,height;
    if(argc!=2||!integer(ctx,argv[0],112,&width)||
       !integer(ctx,argv[1],63,&height)||!width||!height)
        return fail(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"expected dimensions within 112x63");
    bool created=!pixel.open;
    if(pixel.open){
        if(pixel.pool->width!=width||pixel.pool->height!=height)
            return fail(ctx,op,POCKET_ERR_BUSY,"pixel dimensions fixed for this session");
    }else{
        pixel.pool=calloc(1,sizeof *pixel.pool);
        if(!pixel.pool)return fail(ctx,op,POCKET_ERR_OUT_OF_MEMORY,"pixel pool allocation failed");
        if(!ksn_pixel_pool_init(pixel.pool,(uint16_t)width,(uint16_t)height,NULL,0)){
            pocket_pixel_reset();return fail(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"invalid pixel pool");
        }
        pixel.open=true;
    }
    ksn_image_port port={.ctx=&pixel,.width=(uint16_t)width,.height=(uint16_t)height,
        .variants=1,.frames=1,.read_span=read_span,.opaque=false};
    JSValue resource=pocket_kasane_pixel_resource(ctx,&port);
    if(created&&JS_IsException(resource))pocket_pixel_reset();
    return resource;
}
static JSValue stage_pixel(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;const char *op="kasane.pixel.stage";
    unsigned color,alpha;
    if(!pixel.open||argc!=4||!integer(ctx,argv[2],7,&color)||
       !integer(ctx,argv[3],7,&alpha))
        return fail(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"expected open pixel image and register indices");
    if(JS_GetTypedArrayType(argv[0])!=JS_TYPED_ARRAY_UINT16||
       JS_GetTypedArrayType(argv[1])!=JS_TYPED_ARRAY_UINT16)
        return fail(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"expected Uint16Array code and parameters");
    size_t offset=0,bytes=0,element=0;
    JSValue code_buffer=JS_GetTypedArrayBuffer(ctx,argv[0],&offset,&bytes,&element);
    if(JS_IsException(code_buffer))return code_buffer;
    unsigned count=bytes<=80?(unsigned)(bytes/10):0;
    JS_FreeValue(ctx,code_buffer);
    if(!count||bytes%10)
        return fail(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"expected 1 to 8 five-word instructions");
    const uint16_t *code,*params;JSValue cb=JS_UNDEFINED,pb=JS_UNDEFINED;
    if(!words(ctx,argv[0],(size_t)count*5,&code,&cb)||
       !words(ctx,argv[1],KSN_PIXEL_PARAMS,&params,&pb)){
        JS_FreeValue(ctx,cb);
        return fail(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"invalid code or parameter buffer");
    }
    ksn_pixel_frame f={.width=pixel.pool->width,.height=pixel.pool->height,
        .count=(uint8_t)count,.color_reg=(uint8_t)color,.alpha_reg=(uint8_t)alpha};
    memcpy(f.params,params,sizeof f.params);
    for(unsigned i=0;i<count;i++){
        const uint16_t *ins=code+i*5;
        if(ins[0]>UINT8_MAX||ins[1]>UINT8_MAX||ins[2]>UINT8_MAX||ins[3]>UINT8_MAX){
            JS_FreeValue(ctx,cb);JS_FreeValue(ctx,pb);
            return fail(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"instruction field out of range");
        }
        f.code[i]=(ksn_pixel_instruction){(uint8_t)ins[0],(uint8_t)ins[1],
            (uint8_t)ins[2],(uint8_t)ins[3],ins[4]};
        if(f.code[i].op==KSN_PIXEL_UNDERLAY){
            JS_FreeValue(ctx,cb);JS_FreeValue(ctx,pb);
            return fail(ctx,op,POCKET_ERR_UNSUPPORTED,"underlay is not exposed");
        }
    }
    JS_FreeValue(ctx,cb);JS_FreeValue(ctx,pb);
    if(!pocket_kasane_pixel_can_stage())return JS_NewBool(ctx,false);
    ksn_pixel_handle handle;
    ksn_pixel_result result=ksn_pixel_pool_stage(pixel.pool,&f,NULL,&handle);
    if(result==KSN_PIXEL_BUSY)return JS_NewBool(ctx,false);
    if(result!=KSN_PIXEL_OK)
        return fail(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"invalid pixel program");
    pocket_kasane_pixel_invalidate();
    return JS_NewBool(ctx,true);
}
esp_err_t pocket_pixel_install(JSContext *ctx,JSValueConst kasane){
    JSValue object=JS_NewObject(ctx);
    if(JS_IsException(object))return ESP_ERR_NO_MEM;
    static const JSCFunctionListEntry functions[]={
        JS_CFUNC_DEF("open",2,open_pixel),JS_CFUNC_DEF("stage",4,stage_pixel)
    };
    if(JS_SetPropertyFunctionList(ctx,object,functions,2)<0){
        JS_FreeValue(ctx,object);return ESP_ERR_NO_MEM;
    }
    if(JS_SetPropertyStr(ctx,(JSValue)kasane,"pixel",object)<0)return ESP_ERR_NO_MEM;
    return ESP_OK;
}
