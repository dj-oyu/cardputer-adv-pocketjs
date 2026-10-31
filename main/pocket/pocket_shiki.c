#include "pocket_shiki.h"
#include "pocket_api.h"
#include "ksn_shiki.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* run(code, args, inputs): registers code (a Uint8Array in the sec.3 byte
 * format) with args, draws once with inputs, and returns "<hash> <status>":
 * the FNV-1a 32 of the shared text contract's result line (ksn_shiki_result,
 * the line tools/kasane_contract/shiki_run.c prints) in 8 hex digits, and
 * DONE / INVALID / LIMIT / BADINPUT / REJECT. The line itself can be 30 KB
 * (1,024 segments), so the device hashes it in pieces instead of building a
 * guest string; the host (tools/kasane_ir/shiki_device.mjs) hashes the line
 * its own engines print and compares.
 *
 * The plan and the segments (about 11 KB) are allocated per call and freed
 * before returning: the surface exists only in the test app's session, and
 * nothing of it may sit in every build's .bss. */

#define MAX_NUMBERS 64   /* as shiki_run.c: extra inputs reach the engine, which ignores them */

static JSValue fail(JSContext *ctx,const char *code,const char *message){
    return pocket_api_throw(ctx,code,"shikiDebug.run",message,false,POCKET_OUTCOME_NOT_APPLIED);
}
/* An array of at most MAX_NUMBERS numbers. Not coerced: the test is about
 * what JS numbers become (sec.2.4), so a non-number is the caller's mistake. */
static bool numbers(JSContext *ctx,JSValueConst v,double *out,unsigned *n){
    JSValue len=JS_GetPropertyStr(ctx,v,"length");
    uint32_t count=0;
    const bool ok=JS_IsArray(v)&&!JS_IsException(len)&&JS_ToUint32(ctx,&count,len)==0&&count<=MAX_NUMBERS;
    JS_FreeValue(ctx,len);
    if(!ok)return false;
    for(uint32_t k=0;k<count;k++){
        JSValue e=JS_GetPropertyUint32(ctx,v,k);
        const bool good=JS_IsNumber(e)&&JS_ToFloat64(ctx,&out[k],e)==0;
        JS_FreeValue(ctx,e);
        if(!good)return false;
    }
    *n=count;
    return true;
}
static void fnv(void *ctx,const char *text,size_t len){
    uint32_t *h=ctx;
    for(size_t k=0;k<len;k++){*h^=(uint8_t)text[k];*h*=16777619u;}
}
static JSValue js_run(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    size_t len=0;
    uint8_t *code=argc>0?JS_GetUint8Array(ctx,&len,argv[0]):NULL;
    double args[MAX_NUMBERS],in[MAX_NUMBERS];
    unsigned na=0,ni=0;
    if(!code||argc<3||!numbers(ctx,argv[1],args,&na)||!numbers(ctx,argv[2],in,&ni)){
        JS_FreeValue(ctx,JS_GetException(ctx));   /* a non-Uint8Array leaves one pending */
        return fail(ctx,POCKET_ERR_INVALID_ARGUMENT,"expected (Uint8Array, number[], number[])");
    }
    ksn_shiki_plan *plan=malloc(sizeof *plan);
    ksn_proc_segment *seg=malloc(KSN_SHIKI_SEGMENTS*sizeof *seg);
    if(!plan||!seg){free(plan);free(seg);return fail(ctx,POCKET_ERR_OUT_OF_MEMORY,"no room for a run");}
    ksn_shiki_vm vm={0};
    const bool ok=ksn_shiki_register(code,len,args,na,plan);
    const ksn_shiki_status s=ok?ksn_shiki_draw(plan,in,ni>KSN_SHIKI_INPUTS?KSN_SHIKI_INPUTS:ni,&vm,seg)
                               :KSN_SHIKI_INVALID;
    uint32_t h=2166136261u;
    ksn_shiki_result(ok,s,&vm,seg,fnv,&h);
    free(plan);free(seg);
    static const char *const name[]={"RUNNING","DONE","INVALID","LIMIT","BADINPUT"};
    char out[32];
    snprintf(out,sizeof out,"%08lx %s",(unsigned long)h,ok?name[s]:"REJECT");
    return JS_NewString(ctx,out);
}

esp_err_t pocket_shiki_debug_fill(JSContext *ctx,JSValueConst ns,void *unused){
    (void)unused;
    static const JSCFunctionListEntry methods[]={JS_CFUNC_DEF("run",3,js_run)};
    return JS_SetPropertyFunctionList(ctx,ns,methods,1)<0?ESP_ERR_NO_MEM:ESP_OK;
}
/* No limits: the engine's are the spec's (sec.9.1); this surface adds none. */
static const pocket_capability_t capability={.name="kasane.shiki.debug",.supported=true,
    .available=true};
esp_err_t pocket_shiki_debug_install(JSContext *ctx,void *unused){
    (void)unused;
    esp_err_t err=pocket_api_register(&capability);
    return err==ESP_OK?pocket_api_lazy(ctx,"shikiDebug",pocket_shiki_debug_fill,NULL):err;
}
