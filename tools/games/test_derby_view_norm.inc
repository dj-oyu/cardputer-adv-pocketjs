/* Host-only: appended to both revisions of pocket_derby.c by the A/B runner.
 * Exercise the actual ou(), including adjacent representable clip boundaries. */
#include <assert.h>
#include <float.h>
void norm_boundary_test(void){
    derby_course *saved_course=course;
    __typeof__(view) saved_view=view;
    double ve[8]={0};derby_course path={.ve=ve};course=&path;
    const double focal[]={0,-0.0,DBL_TRUE_MIN,200,1500,-200,1e154,DBL_MAX,INFINITY,-INFINITY,NAN};
    const double margin[]={0,-0.0,DBL_TRUE_MIN,30,-30,DBL_MAX,INFINITY,NAN};
    unsigned cases=0;
    for(unsigned i=0;i<sizeof focal/sizeof *focal;i++){
        const double f=focal[i];view.pc[4]=f;
#ifdef NORM_REUSE
        view.ou_norm=sqrt(f*f+25600);
#endif
        for(unsigned j=0;j<sizeof margin/sizeof *margin;j++){
            const double m=margin[j],k=m*sqrt(f*f+25600);
            const double limits[]={.02*f-m,500+m,-k/160,k/160,0,DBL_MAX,-DBL_MAX};
            for(unsigned n=0;n<sizeof limits/sizeof *limits;n++)for(int side=-1;side<=1;side++){
                const double d=side?nextafter(limits[n],side<0?-INFINITY:INFINITY):limits[n];
                ve[0]=ve[4]=d;ve[1]=ve[5]=d;
                const bool expected=(d<.02*f-m&&d<.02*f-m)||(d>500+m&&d>500+m)||
                    (160*d+d*f<-k&&160*d+d*f<-k)||(160*d-d*f<-k&&160*d-d*f<-k);
                assert(ou(0,0,m,500)==expected);++cases;
            }
        }
    }
    course=saved_course;view=saved_view;
    printf("NORM_BOUNDARIES %u PASS\n",cases);
}
static void norm_eval(JSContext *ctx,const char *code){
    JSValue result=JS_Eval(ctx,code,strlen(code),"view-norm-lifecycle.js",JS_EVAL_TYPE_GLOBAL);
    assert(!JS_IsException(result));JS_FreeValue(ctx,result);
}
static JSValue norm_reentrant_ser(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argc;(void)argv;
    /* Outer view has assigned f=1500, but not rebuilt VE yet. The original
     * formula keeps both endpoints inside: -44000 >= -30*sqrt(1500^2+25600). */
    assert(view.pc[4]==1500);assert(!ou(0,0,30,500));
    return JS_NewInt32(ctx,10);
}
static void norm_check_view(void){
    assert(course&&view.set);
#ifdef NORM_REUSE
    const double f=view.pc[4],expected=sqrt(f*f+25600);
    assert(memcmp(&expected,&view.ou_norm,sizeof expected)==0);
#endif
}
void norm_lifecycle_test(JSContext *ctx){
    norm_eval(ctx,"__pd.course([0,100],[[100,0,0,0,1,0,0]],0);__pd.view([0,0,1,0,200,10],500,0,0,0,0,0)");
    norm_check_view();assert(view.pc[4]==200);
    norm_eval(ctx,"__pd.view([0,0,1,0,1500,10],500,0,0,0,0,0)");
    norm_check_view();assert(view.pc[4]==1500);
    /* A late getter replaces the camera, after outer camera f was read. */
    norm_eval(ctx,"__pd.view(Object.defineProperty([0,0,1,0,200,10],5,{get(){__pd.view([0,0,1,0,777,10],500,0,0,0,0,0);return 10}}),500,0,0,0,0,0)");
    norm_check_view();assert(view.pc[4]==777);
    norm_eval(ctx,"(()=>{let caught=false;try{__pd.view([0,0,1,0,200,{}],500,0,0,0,0,0)}catch(e){caught=true}if(!caught)throw Error('accepted invalid view')})()");
    assert(!view.set);
    norm_eval(ctx,"__pd.course([0,100],0,0);__pd.view([0,0,1,0,200,10],500,0,0,0,0,0)");
    assert(course&&view.set&&!course->sections);
    norm_eval(ctx,"__pd.course([0,100],[[100,0,0,0,1,0,0]],0);__pd.view([0,0,1,0,333,10],500,0,0,0,0,0)");
    norm_check_view();assert(view.pc[4]==333);
    JSValue global=JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx,global,"__normGetter",JS_NewCFunction(ctx,norm_reentrant_ser,"__normGetter",0));
    norm_eval(ctx,"__pd.course([0,1],[[100,0,100,40,1,0,0]],0);__pd.view([0,0,1,0,200,10],500,0,0,0,0,0);__pd.view(Object.defineProperty([0,0,1,0,1500,10],5,{get:__normGetter}),500,0,0,0,0,0)");
    norm_check_view();
    JS_SetPropertyStr(ctx,global,"__normGetter",JS_UNDEFINED);JS_FreeValue(ctx,global);
    pocket_derby_reset();assert(!course&&!view.set);
    puts("NORM_LIFECYCLE PASS");
}
