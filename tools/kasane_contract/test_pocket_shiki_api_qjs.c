/* pocket.kasane.shiki on the host: real QuickJS, the real pocket_proc.c,
 * pocket_shiki_api.c, shiki_heap.c and ksn_shiki.c, with the presenter
 * stubbed. Runs apps/shikitest/shiki_api_test.js (the device's own suite)
 * and fails on any of its checks. run_pocket_shiki_api_qjs.py builds it. */
#include "quickjs.h"
#include "pocket_proc.h"
#include "pocket_api.h"
#include "pocket_shiki_api.h"
#include "shiki_heap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(x) do { if(!(x)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);} } while(0)

ksn_result pocket_kasane_proc_publish(void){return KSN_OK;}
ksn_result pocket_kasane_proc_publish_at(unsigned surface,ksn_rect damage){(void)surface;(void)damage;return KSN_OK;}
void pocket_kasane_invalidate(void){}
JSValue pocket_kasane_proc_resource(JSContext *ctx){pocket_proc_image_mode();return JS_NewObject(ctx);}
JSValue pocket_kasane_proc_resource_at(JSContext *ctx,unsigned surface){pocket_proc_image_mode_at(surface);return JS_NewObject(ctx);}
/* The code first in the message, as the suite reads it when e.code is absent. */
JSValue pocket_api_throw(JSContext *ctx,const char *code,const char *op,
                         const char *message,bool retryable,const char *outcome){
    (void)retryable;(void)outcome;
    JSValue e=JS_NewError(ctx);
    char text[256];snprintf(text,sizeof text,"%s %s: %s",code,op,message);
    JS_SetPropertyStr(ctx,e,"message",JS_NewString(ctx,text));
    JS_SetPropertyStr(ctx,e,"code",JS_NewString(ctx,code));
    return JS_Throw(ctx,e);
}
esp_err_t pocket_api_register(const pocket_capability_t *capability){(void)capability;return ESP_OK;}

static JSValue js_log(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    for(int k=0;k<argc;k++){const char *s=JS_ToCString(ctx,argv[k]);fprintf(stderr,"%s\n",s?s:"?");JS_FreeCString(ctx,s);}
    return JS_UNDEFINED;
}
static char *read_file(const char *path,size_t *size){
    FILE *f=fopen(path,"rb");REQUIRE(f);
    REQUIRE(fseek(f,0,SEEK_END)==0);long n=ftell(f);REQUIRE(n>0);REQUIRE(fseek(f,0,SEEK_SET)==0);
    char *data=malloc((size_t)n+1);REQUIRE(data);
    REQUIRE(fread(data,1,(size_t)n,f)==(size_t)n);fclose(f);data[n]=0;*size=(size_t)n;return data;
}
static void check(JSContext *ctx,JSValue v){
    if(JS_IsException(v)){
        JSValue e=JS_GetException(ctx);const char *m=JS_ToCString(ctx,e);
        fprintf(stderr,"QuickJS: %s\n",m?m:"exception");exit(1);
    }
}
int main(int argc,char **argv){
    REQUIRE(argc==2);
    REQUIRE(shiki_heap_begin(0));
    JSRuntime *rt=JS_NewRuntime();REQUIRE(rt);
    JSContext *ctx=JS_NewContext(rt);REQUIRE(ctx);
    JSValue global=JS_GetGlobalObject(ctx),pocket=JS_NewObject(ctx),kasane=JS_NewObject(ctx);
    REQUIRE(JS_SetPropertyStr(ctx,kasane,"grid",JS_NewObject(ctx))>=0);
    REQUIRE(pocket_shiki_api_install(ctx,kasane)==ESP_OK);
    REQUIRE(JS_SetPropertyStr(ctx,pocket,"kasane",kasane)>=0);
    REQUIRE(JS_SetPropertyStr(ctx,global,"pocket",JS_DupValue(ctx,pocket))>=0);
    REQUIRE(JS_SetPropertyStr(ctx,global,"log",JS_NewCFunction(ctx,js_log,"log",1))>=0);
    size_t n;char *src=read_file(argv[1],&n);
    JSValue loaded=JS_Eval(ctx,src,n,argv[1],JS_EVAL_TYPE_GLOBAL);
    check(ctx,loaded);JS_FreeValue(ctx,loaded);
    JSValue r=JS_Eval(ctx,"shikiApiTest(pocket, log)",25,"run",JS_EVAL_TYPE_GLOBAL);
    check(ctx,r);
    int32_t pass=0,fail=0;
    JSValue p=JS_GetPropertyUint32(ctx,r,0),f=JS_GetPropertyUint32(ctx,r,1);
    JS_ToInt32(ctx,&pass,p);JS_ToInt32(ctx,&fail,f);
    JS_FreeValue(ctx,p);JS_FreeValue(ctx,f);JS_FreeValue(ctx,r);
    /* The session's end, in app_stop()'s order. */
    pocket_proc_reset();
    pocket_shiki_api_reset();
    JS_FreeValue(ctx,pocket);JS_FreeValue(ctx,global);
    JS_FreeContext(ctx);JS_FreeRuntime(rt);
    shiki_heap_end();
    uint32_t bytes,used,largest;shiki_heap_info(&bytes,&used,&largest);
    printf("shiki api: pass %d fail %d; heap after the session: %s\n",pass,fail,bytes?"STILL OPEN":"released");
    free(src);
    return fail||bytes?1:0;
}
