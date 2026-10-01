/* Isolated boundary prototype, not a public grid API or kernel benchmark. */
#include "quickjs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"borrow test failed line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static size_t copy_peak;
static unsigned aligned, unaligned;
static JSValue sum_inputs(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv,int copy){
    (void)self;
    JSValue values[2]={JS_UNDEFINED,JS_UNDEFINED},buffers[2]={JS_UNDEFINED,JS_UNDEFINED};
    int16_t *owned[2]={NULL,NULL};const int16_t *data[2]={NULL,NULL};size_t counts[2]={0,0};
    JSValue result=JS_EXCEPTION;
    if(argc!=1||!JS_IsObject(argv[0]))return JS_ThrowTypeError(ctx,"input map");
    /* Get every value before deriving pointers: a later getter may detach an
     * earlier buffer. Keep returned ephemeral views rooted through execution. */
    for(unsigned i=0;i<2;i++){
        values[i]=JS_GetPropertyUint32(ctx,argv[0],i);
        if(JS_IsException(values[i]))goto done;
    }
    size_t copied=0,total_count=0;
    for(unsigned i=0;i<2;i++){
        if(JS_IsNull(values[i])||JS_IsUndefined(values[i]))continue;
        if(JS_GetTypedArrayType(values[i])!=JS_TYPED_ARRAY_INT16){
            JS_ThrowTypeError(ctx,"expected Int16Array");goto done;
        }
        size_t offset=0,bytes=0,per=0,total=0;
        buffers[i]=JS_GetTypedArrayBuffer(ctx,values[i],&offset,&bytes,&per);
        if(!JS_IsArrayBuffer(buffers[i])){
            JS_ThrowTypeError(ctx,"shared input excluded");goto done;
        }
        uint8_t *raw=JS_IsException(buffers[i])?NULL:JS_GetArrayBuffer(ctx,&total,buffers[i]);
        if(!raw||per!=2||bytes%2||offset>total||bytes>total-offset){
            JS_ThrowTypeError(ctx,"detached or invalid input");goto done;
        }
        counts[i]=bytes/2;
        if(counts[i]>8192-total_count){JS_ThrowTypeError(ctx,"input limit");goto done;}
        total_count+=counts[i];
        data[i]=(const int16_t *)(raw+offset);
        if(copy){
            owned[i]=malloc(bytes?bytes:1);if(!owned[i]){JS_ThrowOutOfMemory(ctx);goto done;}
            memcpy(owned[i],data[i],bytes);data[i]=owned[i];copied+=bytes;
        }else if((uintptr_t)data[i]&15u)unaligned++;else aligned++;
    }
    if(copied>copy_peak)copy_peak=copied;
    int32_t sum=0;
    /* No JS call occurs between pointer acquisition and consumption. */
    for(unsigned i=0;i<2;i++)for(size_t j=0;j<counts[i];j++)sum+=data[i][j];
    result=JS_NewInt32(ctx,sum);
done:
    for(unsigned i=0;i<2;i++){free(owned[i]);JS_FreeValue(ctx,buffers[i]);JS_FreeValue(ctx,values[i]);}
    return result;
}
static void eval(JSContext *ctx,const char *s){
    JSValue r=JS_Eval(ctx,s,strlen(s),"borrow.js",JS_EVAL_TYPE_GLOBAL);
    if(JS_IsException(r)){
        JSValue e=JS_GetException(ctx);const char *msg=JS_ToCString(ctx,e);
        fprintf(stderr,"borrow JS error: %s\n",msg?msg:"unknown");exit(1);
    }
    JS_FreeValue(ctx,r);
}
int main(void){
    JSRuntime *rt=JS_NewRuntime();CHECK(rt);JSContext *ctx=JS_NewContext(rt);CHECK(ctx);
    JSValue global=JS_GetGlobalObject(ctx);
    CHECK(JS_SetPropertyStr(ctx,global,"copySum",JS_NewCFunctionMagic(ctx,sum_inputs,"copySum",1,JS_CFUNC_generic_magic,1))==1);
    CHECK(JS_SetPropertyStr(ctx,global,"borrowSum",JS_NewCFunctionMagic(ctx,sum_inputs,"borrowSum",1,JS_CFUNC_generic_magic,0))==1);
    JS_FreeValue(ctx,global);
    eval(ctx,"function check(v){if(!v)throw Error('mismatch');}"
        "let a=new Int16Array(8192).fill(1);check(copySum({0:a})===8192);check(borrowSum({0:a})===8192);"
        "for(let off=0;off<16;off+=2){let b=new ArrayBuffer(64);let v=new Int16Array(b,off,8).fill(3);"
        "check(copySum({0:v,1:v})===borrowSum({0:v,1:v}));}"
        "check(borrowSum({get 0(){return new Int16Array(8).fill(7)},get 1(){return new Int16Array(8).fill(-3)}})===32);"
        "for(let i=0;i<600;i++)check(borrowSum({0:a})===copySum({0:a}));"
        "let detached=new Int16Array(8);let rejected=false;try{borrowSum({0:detached,get 1(){detached.buffer.transfer();return null}})}catch(e){rejected=true;}check(rejected);"
        "let rejectedType=false;try{borrowSum({0:new Uint8Array(8)})}catch(e){rejectedType=true;}check(rejectedType);"
        "if(typeof SharedArrayBuffer!=='undefined'){let rejectedShared=false;try{borrowSum({0:new Int16Array(new SharedArrayBuffer(16))})}catch(e){rejectedShared=true;}check(rejectedShared);}");
    CHECK(copy_peak==16384);CHECK(aligned+unaligned>0);CHECK(unaligned>0);
    printf("borrow boundary PASS: copy peak=%zu B, borrow native input=0 B; offsets/alias/ephemeral views/late getter detach/type/600 cycles; aligned=%u unaligned=%u\n",copy_peak,aligned,unaligned);
    JS_FreeContext(ctx);JS_FreeRuntime(rt);
}
