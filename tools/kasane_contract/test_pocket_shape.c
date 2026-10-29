/* pocket.shapes against its contract (docs/api/shapes.md): the segments each
 * call draws for known shapes, the options, the errors, and that a call
 * stopped by the frame limit leaves nothing behind. The frame is a recorder
 * standing in for pocket_proc.c's native lines (same limit semantics: a
 * line past the limit throws LIMIT_EXCEEDED, the caller rolls back). Host
 * only: tools/kasane_contract/run_pocket_shape.py. */
#include "quickjs.h"
#include "pocket_api.h"
#include "pocket_proc.h"
#include "pocket_shape.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(x) do{if(!(x)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}}while(0)

static ksn_proc_segment segs[4096];
static unsigned nsegs,limit=4096;
static bool building=true;
JSValue pocket_proc_native_begin(JSContext *ctx,const char *op,pocket_proc_mark *mark){
    if(!building)return pocket_api_throw(ctx,POCKET_ERR_BUSY,op,"beginFrame required",false,NULL);
    *mark=(pocket_proc_mark){(uint16_t)nsegs,0,0,0};
    return JS_UNDEFINED;
}
JSValue pocket_proc_native_line(JSContext *ctx,const char *op,int x0,int y0,int x1,int y1,uint16_t color){
    if(nsegs>=limit){
        building=false;
        return pocket_api_throw(ctx,POCKET_ERR_LIMIT_EXCEEDED,op,"frame drawing limit",false,NULL);
    }
    REQUIRE(x0>=-480&&x0<=720&&y0>=-480&&y0<=720&&x1>=-480&&x1<=720&&y1>=-480&&y1<=720);
    segs[nsegs++]=(ksn_proc_segment){(int16_t)x0,(int16_t)y0,(int16_t)x1,(int16_t)y1,color};
    return JS_UNDEFINED;
}
void pocket_proc_native_rollback(const pocket_proc_mark *mark){nsegs=mark->count;}
JSValue pocket_proc_native_ellipsoid(JSContext *ctx,const char *op,const ksn_proc_ellipsoid *patch){
    (void)patch;
    return pocket_api_throw(ctx,POCKET_ERR_UNSUPPORTED,op,"analytic patches need the renderer harness",false,NULL);
}

/* __segs(): the recorded segments as "x0,y0,x1,y1,c;..." and cleared;
 * __limit(n) caps them; __begin() opens a frame again. */
static JSValue js_segs(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argc;(void)argv;
    static char s[65536];size_t o=0;
    for(unsigned i=0;i<nsegs&&o<sizeof s-64;i++)
        o+=(size_t)snprintf(s+o,sizeof s-o,"%s%d,%d,%d,%d,%u",i?";":"",segs[i].x0,segs[i].y0,
                            segs[i].x1,segs[i].y1,segs[i].color);
    s[o]=0;nsegs=0;
    return JS_NewString(ctx,s);
}
static JSValue js_limit(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argc;int32_t n;JS_ToInt32(ctx,&n,argv[0]);limit=(unsigned)n;return JS_UNDEFINED;
}
static JSValue js_begin(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)ctx;(void)self;(void)argc;(void)argv;building=true;return JS_UNDEFINED;
}
static char *read_file(const char *path,size_t *size){
    FILE *f=fopen(path,"rb");REQUIRE(f);
    fseek(f,0,SEEK_END);long n=ftell(f);fseek(f,0,SEEK_SET);
    char *s=malloc((size_t)n+1);REQUIRE(s&&fread(s,1,(size_t)n,f)==(size_t)n);
    s[n]=0;fclose(f);*size=(size_t)n;return s;
}
int main(int argc,char **argv){
    REQUIRE(argc==2);
    JSRuntime *rt=JS_NewRuntime();JSContext *ctx=JS_NewContext(rt);
    JSValue g=JS_GetGlobalObject(ctx),shapes=JS_NewObject(ctx);
    REQUIRE(pocket_shape_fill(ctx,shapes,NULL)==ESP_OK);
    JS_SetPropertyStr(ctx,g,"shapes",shapes);
    JS_SetPropertyStr(ctx,g,"__segs",JS_NewCFunction(ctx,js_segs,"__segs",0));
    JS_SetPropertyStr(ctx,g,"__limit",JS_NewCFunction(ctx,js_limit,"__limit",1));
    JS_SetPropertyStr(ctx,g,"__begin",JS_NewCFunction(ctx,js_begin,"__begin",0));
    JS_FreeValue(ctx,g);
    size_t n;char *src=read_file(argv[1],&n);
    JSValue r=JS_Eval(ctx,src,n,argv[1],JS_EVAL_TYPE_GLOBAL);
    if(JS_IsException(r)){
        JSValue e=JS_GetException(ctx);const char *m=JS_ToCString(ctx,e);
        JSValue st=JS_GetPropertyStr(ctx,e,"stack");const char *sm=JS_ToCString(ctx,st);
        fprintf(stderr,"FAIL pocket.shapes contract: %s\n%s\n",m?m:"?",sm?sm:"");
        return 1;
    }
    JS_FreeValue(ctx,r);free(src);
    JS_FreeContext(ctx);JS_FreeRuntime(rt);
    printf("PASS pocket.shapes contract\n");
    return 0;
}
