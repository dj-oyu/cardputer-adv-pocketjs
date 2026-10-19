#include "quickjs.h"
#include "derby_telemetry.h"
#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {size_t n; max_align_t align;} header;
static size_t allocations;
static void *alloc(void *o,size_t n){(void)o;header *p=malloc(sizeof *p+n);if(!p)return NULL;p->n=n;++allocations;return p+1;}
static void *zalloc(void *o,size_t c,size_t n){if(n&&c>SIZE_MAX/n)return NULL;void*p=alloc(o,c*n);if(p)memset(p,0,c*n);return p;}
static void release(void *o,void *p){(void)o;if(p)free((header*)p-1);}
static void *resize(void *o,void *p,size_t n){if(!p)return alloc(o,n);if(!n){release(o,p);return NULL;}header*q=realloc((header*)p-1,sizeof(header)+n);if(!q)return NULL;q->n=n;++allocations;return q+1;}
static size_t usable(const void *p){return p?((const header*)p-1)->n:0;}
static const JSMallocFunctions mf={zalloc,alloc,release,resize,usable};
bool derby_telemetry_write(const char *p,size_t n){(void)p;(void)n;return true;}
#define POCKET_ERR_INVALID_ARGUMENT "INVALID_ARGUMENT"
static JSValue fail(JSContext *ctx,const char *op,const char *code,const char *message){(void)op;(void)code;return JS_ThrowTypeError(ctx,"%s",message);}
#include "derby_context.inc"
int main(void){
    JSRuntime *rt=JS_NewRuntime2(&mf,NULL);assert(rt);
    JSContext *ctx=JS_NewContext(rt);assert(ctx);
    JSValue fn=JS_NewCFunction(ctx,context_call,"view",7),args[14];
    for(unsigned i=0;i<14;++i)args[i]=JS_NewInt32(ctx,0);
    args[7]=JS_NewInt32(ctx,2);args[8]=JS_NewInt32(ctx,1);args[9]=JS_NewInt32(ctx,2);
    args[10]=JS_NewInt32(ctx,8);args[11]=JS_NewInt32(ctx,391);args[12]=JS_NewFloat64(ctx,.5);args[13]=JS_NewInt32(ctx,1);
    for(unsigned i=0;i<32;++i){JSValue v=JS_Call(ctx,fn,JS_UNDEFINED,14,args);assert(!JS_IsException(v));JS_FreeValue(ctx,v);}
    size_t before=allocations;
    for(unsigned i=0;i<10000;++i){
        derby_telemetry_begin_frame();
        JSValue v=JS_Call(ctx,fn,JS_UNDEFINED,14,args);assert(!JS_IsException(v));JS_FreeValue(ctx,v);
    }
    assert(allocations==before);
    for(unsigned i=0;i<14;++i)JS_FreeValue(ctx,args[i]);
    JS_FreeValue(ctx,fn);JS_FreeContext(ctx);JS_FreeRuntime(rt);
    puts("PASS real QuickJS native context call: 10000 calls, zero allocator calls");
}
