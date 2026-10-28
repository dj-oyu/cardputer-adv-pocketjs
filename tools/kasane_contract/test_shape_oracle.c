/* pocket.shapes against the JS it replaced (docs/apps/megademo-shapes.md).
 * Runs MEGADEMO's WAKE in two QuickJS contexts: the reference (argv[1], the
 * chunks as they were before pocket.shapes, which drew in JS) and the
 * candidate (argv[2], the chunks of the tree, drawing through pocket.shapes
 * linked here). Every frame's draws (plan, inputs) are compared. Built with
 * POCKET_SHAPE_DOUBLE the C computes in double as the JS did and every input
 * must be bit-identical; built without (float, as on the device) the report
 * gives the largest difference and how many draws differ, and the scenes
 * runner's pixels say what that does to the picture. Host only. */
#include "quickjs.h"
#include "pocket_shape.h"
#include "pocket_proc.h"
#include "shape_draw_shim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(x) do{if(!(x)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}}while(0)

static char *read_file(const char *path,size_t *size){
    FILE *f=fopen(path,"rb");REQUIRE(f);
    fseek(f,0,SEEK_END);long n=ftell(f);fseek(f,0,SEEK_SET);
    char *s=malloc((size_t)n+1);REQUIRE(s&&fread(s,1,(size_t)n,f)==(size_t)n);
    s[n]=0;fclose(f);*size=(size_t)n;return s;
}
static void eval_or_die(JSContext *ctx,const char *src,size_t n,const char *name){
    JSValue r=JS_Eval(ctx,src,n,name,JS_EVAL_TYPE_GLOBAL);
    if(JS_IsException(r)){
        JSValue e=JS_GetException(ctx);const char *m=JS_ToCString(ctx,e);
        fprintf(stderr,"%s: %s\n",name,m?m:"?");exit(1);
    }
    JS_FreeValue(ctx,r);
}
static void eval_files(JSContext *ctx,const char *list){
    char buf[4096];snprintf(buf,sizeof buf,"%s",list);
    for(char *file=strtok(buf,",");file;file=strtok(NULL,",")){
        size_t n;char *src=read_file(file,&n);eval_or_die(ctx,src,n,file);free(src);
    }
}
/* WAKE's frames as JSON: [[plan, [inputs]], ...] a frame. */
static const char DUMP[]=
    "globalThis.__dump=JSON.stringify(Array.from({length:procMegademo.len[7]},"
    "(_,t)=>procMegademo.frame(7,2,t).d))";
static char *dump(JSContext *ctx){
    eval_or_die(ctx,DUMP,sizeof DUMP-1,"dump.js");
    JSValue g=JS_GetGlobalObject(ctx),v=JS_GetPropertyStr(ctx,g,"__dump");
    const char *s=JS_ToCString(ctx,v);REQUIRE(s);
    char *copy=strdup(s);
    JS_FreeCString(ctx,s);JS_FreeValue(ctx,v);JS_FreeValue(ctx,g);
    return copy;
}
int main(int argc,char **argv){
    REQUIRE(argc==3);
    JSRuntime *ra=JS_NewRuntime();JSContext *a=JS_NewContext(ra);
    eval_files(a,argv[1]);
    char *ref=dump(a);
    JS_FreeContext(a);JS_FreeRuntime(ra);

    JSRuntime *rb=JS_NewRuntime();JSContext *b=JS_NewContext(rb);
    JSValue g=JS_GetGlobalObject(b),pocket=JS_NewObject(b),shapes=JS_NewObject(b);
    REQUIRE(pocket_shape_fill(b,shapes,NULL)==ESP_OK);
    JS_SetPropertyStr(b,pocket,"shapes",shapes);
    JS_SetPropertyStr(b,g,"pocket",pocket);
    JS_SetPropertyStr(b,g,"__ref",JS_NewString(b,ref));
    JS_FreeValue(b,g);
    static const char draw[]=SHAPE_DRAW_JS;
    eval_or_die(b,draw,sizeof draw-1,"shim.js");
    eval_files(b,argv[2]);
    char *cand=dump(b);
    /* Plans and counts must match; inputs are compared exactly, as float32
     * (what the VM reads), and by their largest difference. */
    static const char cmp[]=
        "{const R=JSON.parse(__ref),C=JSON.parse(__cand);let fr=0,dr=0,exact=0,f32=0,max=0,cnt=0,"
        "where='';"
        "for(let t=0;t<R.length;t++){const r=R[t],c=C[t];"
        "if(r.length!==c.length){if(!cnt)where='frame '+t+': '+r.length+' vs '+c.length+' draws';cnt++;continue}"
        "fr++;for(let i=0;i<r.length;i++){dr++;"
        "if(r[i][0]!==c[i][0]||r[i][1].length!==c[i][1].length)throw Error('plan differs at '+t+'/'+i);"
        "let e=1,f=1;for(let j=0;j<r[i][1].length;j++){const x=r[i][1][j],y=c[i][1][j];"
        "if(x!==y)e=0;if(Math.fround(x)!==Math.fround(y))f=0;"
        "const d=Math.abs(x-y);if(d>max){max=d;if(d>.5)where='frame '+t+' draw '+i+' input '+j+': '+x+' vs '+y}}"
        "exact+=e;f32+=f}}"
        "globalThis.__report=[R.length,fr,cnt,dr,exact,f32,max,where];"
        "if(globalThis.__dbg!==undefined){const t=__dbg;for(let i=0;i<Math.max(R[t].length,C[t].length);i++){const x=JSON.stringify(R[t][i]),y=JSON.stringify(C[t][i]);if(x!==y){__out+=i+' R '+x+'\\n'+i+' C '+y+'\\n'}}}}";
    JSValue gb=JS_GetGlobalObject(b);
    JS_SetPropertyStr(b,gb,"__cand",JS_NewString(b,cand));
    JS_SetPropertyStr(b,gb,"__out",JS_NewString(b,""));
    if(getenv("ORACLE_DBG"))JS_SetPropertyStr(b,gb,"__dbg",JS_NewInt32(b,atoi(getenv("ORACLE_DBG"))));
    eval_or_die(b,cmp,sizeof cmp-1,"compare.js");
    if(getenv("ORACLE_EVAL")){const char *e=getenv("ORACLE_EVAL");eval_or_die(b,e,strlen(e),"dbg.js");}
    JSValue dbg=JS_GetPropertyStr(b,gb,"__out");
    const char *ds=JS_ToCString(b,dbg);
    if(ds&&*ds)fputs(ds,stdout);
    JS_FreeCString(b,ds);JS_FreeValue(b,dbg);
    JSValue rep=JS_GetPropertyStr(b,gb,"__report");
    const char *s=JS_ToCString(b,rep);
    printf("frames,same-count frames,count mismatches,draws,exact,float32-equal,max |diff|,where: %s\n",s);
    int fails=0;
    double v[8];
    for(int i=0;i<7;i++){JSValue e=JS_GetPropertyUint32(b,rep,i);JS_ToFloat64(b,&v[i],e);JS_FreeValue(b,e);}
#ifdef POCKET_SHAPE_DOUBLE
    fails=v[2]!=0||v[4]!=v[3];
    printf("%s shapes oracle (double): %g frames, %g draws, %g bit-identical\n",fails?"FAIL":"PASS",v[0],v[3],v[4]);
#else
    printf("INFO shapes oracle (float, the device build): %g of %g frames keep the draw count; "
           "%g of %g draws float32-equal; largest input difference %g\n",v[1],v[0],v[5],v[3],v[6]);
#endif
    JS_FreeCString(b,s);JS_FreeValue(b,rep);JS_FreeValue(b,gb);
    free(ref);free(cand);
    JS_FreeContext(b);JS_FreeRuntime(rb);
    return fails;
}
