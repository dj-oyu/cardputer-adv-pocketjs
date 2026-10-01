#include "pocket_derby.h"
#include "pocket_api.h"
#include "pocket_proc.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* DERBY WATCH's panning series (ser() in apps/derby/derby_pan.js as of vm/main
 * fac3552) translated to C, docs/apps/derby-ser-native.md. The JS is the
 * specification and this file must produce the very same draw() inputs, bit
 * for bit: every expression keeps the JS's operation order, in double (soft
 * float on the device, as QuickJS's numbers are), with QuickJS's own
 * Math.round/min/max and the libm floor/ceil/sqrt/sin/cos/fmod QuickJS calls.
 * The build keeps the compiler from fusing or reordering (-ffp-contract=off,
 * main/CMakeLists.txt). The host harness checks it against the JS on every
 * frame and on random cameras (tools/games/derby_ser_oracle.js).
 *
 * Why C: the series were 27.6 of the oval bend's 44.25 ms JS turn on the
 * device (docs/apps/derby-native-survey.md). The plans, the VM and the draw
 * path are untouched: each draw goes through pocket_proc_draw_numbers(), the
 * second half of H.draw(). */

#define DERBY_ENDS_MAX 32u
#define DERBY_SECTIONS_MAX 4u
#define SECTION_TERMS 7u    /* length, curvature, x, z, tangent x, z, heading */

/* One heap block per course() (freed by course() again and reset()): the
 * chord ends (CH), the sections (OC, none for the straight, whose pose() is
 * [g, w, 1, 0]), and the per-frame chord ends seen from the camera (VE, four
 * a chord end) with the chords that may be in view (VC). */
typedef struct {
    unsigned ends,sections,in_view;
    double dnr;
    double *ch,*sec,*ve;
    uint8_t *vc;
} derby_course;
static derby_course *course;
/* view(): the camera pc = [x, z, unit x, unit z, f, distance], the far depth,
 * the scene frame t, the stands' plans and their two constants. */
static struct {
    double pc[6],zf,t,hl_color,cells;
    uint32_t hl,crowd;
    bool set;
} view;

/* ---- QuickJS's Math, as quickjs.c has it (js_math_round, js_fmin, js_fmax,
 * js_math_min_max): signed zeros and NaN behave as in the JS. */
typedef union { double d; uint64_t u; } f64;
static double js_round(double a){
    f64 u={.d=a};
    const unsigned e=(unsigned)(u.u>>52)&0x7ffu;
    if(e<1023u){
        if(e==1022u&&u.u!=0xbfe0000000000000ull)u.u=(u.u&(1ull<<63))|(1023ull<<52);
        else u.u&=1ull<<63;
    }else if(e<1023u+52u){
        const unsigned s=(unsigned)(u.u>>63);
        const uint64_t one=1ull<<(52u-(e-1023u));
        u.u+=(one>>1)-s;
        u.u&=~(one-1u);
    }
    return u.d;
}
static double js_min(double a,double b){
    if(isnan(a))return a;
    if(isnan(b))return b;
    if(a==0&&b==0){f64 x={.d=a},y={.d=b};x.u|=y.u;return x.d;}
    return a<b?a:b;
}
static double js_max(double a,double b){
    if(isnan(a))return a;
    if(isnan(b))return b;
    if(a==0&&b==0){f64 x={.d=a},y={.d=b};x.u&=y.u;return x.d;}
    return a<b?b:a;
}
/* ToInt32 (only the L = 0 series' parity reads it). */
static int32_t js_int32(double d){
    if(!isfinite(d))return 0;
    d=trunc(d);
    double m=fmod(d,4294967296.0);
    if(m<0)m+=4294967296.0;
    return (int32_t)(uint32_t)m;
}

/* pose(g, w) of derby_view.js: the point g m along the course, w m out. The
 * JS loop's bound 2 is its section count less one. */
static void pose(double g,double w,double o[4]){
    if(!course->sections){o[0]=g;o[1]=w;o[2]=1;o[3]=0;return;}
    unsigned i=0;
    while(i<course->sections-1u&&g>course->sec[SECTION_TERMS*i])g-=course->sec[SECTION_TERMS*i++];
    const double *e=course->sec+SECTION_TERMS*i;
    double x=e[2],z=e[3],c=e[4],d=e[5];
    if(e[1]!=0){
        const double r=1/e[1]+course->dnr,C=cos(e[6]+e[1]*g),S=sin(e[6]+e[1]*g);
        x+=(S-d)*r;z-=(C-c)*r;c=C;d=S;g=0;
    }
    o[0]=x+g*c-w*d;o[1]=z+g*d+w*c;o[2]=c;o[3]=d;
}
/* ou(j, w, m, zf): chord j at depth w wholly out of view by m m at least. */
static bool ou(unsigned j,double w,double m,double zf){
    const double *V=course->ve+4u*j,f=view.pc[4],k=m*sqrt(f*f+25600),
        l=V[0]+w*V[2],d=V[1]+w*V[3],L=V[4]+w*V[6],D=V[5]+w*V[7];
    return (d<.02*f-m&&D<.02*f-m)||(d>zf+m&&D>zf+m)||(160*d+l*f<-k&&160*D+L*f<-k)||
           (160*d-l*f<-k&&160*D-L*f<-k);
}
static bool inr(const double q[2],double U,double V,double e){
    const double z=q[1]+e*U,x=(q[0]+e*V)/z;
    return z>.02&&x>-400&&x<640;
}
/* lim(c0, c1): narrows lo..hi to c0 + c1 g >= 0. */
static void lim(double *lo,double *hi,double c0,double c1){
    if(c1>0)*lo=js_max(*lo,-c0/c1);
    else if(c1<0)*hi=js_min(*hi,-c0/c1);
    else if(c0<0)*hi=-1e9;
}

static JSValue fail(JSContext *ctx,const char *op,const char *code,const char *message){
    return pocket_api_throw(ctx,code,op,message,false,POCKET_OUTCOME_NOT_APPLIED);
}
static bool number(JSContext *ctx,JSValueConst v,double *out){
    return JS_IsNumber(v)&&JS_ToFloat64(ctx,out,v)==0;
}
/* A plan handle as dr() reads live[n]: falsy draws nothing (0), else it must
 * be a handle-shaped integer (pocket_proc_draw_numbers looks it up). */
static bool plan_handle(JSContext *ctx,JSValueConst v,uint32_t *out){
    double n;
    *out=0;
    if(!JS_ToBool(ctx,v))return true;
    if(!number(ctx,v,&n)||n<1||n>INT32_MAX||n!=floor(n))return false;
    *out=(uint32_t)n;return true;
}
static bool element(JSContext *ctx,JSValueConst array,uint32_t i,double *out){
    JSValue v=JS_GetPropertyUint32(ctx,array,i);
    const bool ok=!JS_IsException(v)&&number(ctx,v,out);
    JS_FreeValue(ctx,v);
    return ok;
}
static bool length_of(JSContext *ctx,JSValueConst array,uint32_t *out){
    double n;
    if(!JS_IsArray(array))return false;
    JSValue v=JS_GetPropertyStr(ctx,array,"length");
    const bool ok=!JS_IsException(v)&&number(ctx,v,&n)&&n>=0&&n<=UINT32_MAX;
    JS_FreeValue(ctx,v);
    if(ok)*out=(uint32_t)n;
    return ok;
}
static JSValue draw(JSContext *ctx,uint32_t handle,const double in[8]){
    return handle?pocket_proc_draw_numbers(ctx,handle,in,8):JS_UNDEFINED;
}

/* course(ends, sections, dnr): CH, OC (or falsy: the straight) and DNR. */
static JSValue js_course(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *op="derby.course";
    uint32_t ends=0,sections=0;
    double dnr;
    if(argc<3||!length_of(ctx,argv[0],&ends)||ends<2||
       (JS_ToBool(ctx,argv[1])&&(!length_of(ctx,argv[1],&sections)||!sections))||
       !number(ctx,argv[2],&dnr)||!isfinite(dnr))
        return fail(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"expected chord ends, sections or 0, and a depth");
    if(ends>DERBY_ENDS_MAX||sections>DERBY_SECTIONS_MAX)
        return fail(ctx,op,POCKET_ERR_LIMIT_EXCEEDED,"32 chord ends and 4 sections maximum");
    /* Read into a new block first: a bad argument leaves the old course. */
    const size_t doubles=ends+(size_t)SECTION_TERMS*sections+4u*ends;
    derby_course *c=malloc(sizeof *c+doubles*sizeof(double)+ends);
    if(!c)return fail(ctx,op,POCKET_ERR_OUT_OF_MEMORY,"course allocation failed");
    c->ends=ends;c->sections=sections;c->in_view=0;c->dnr=dnr;
    c->ch=(double *)(c+1);c->sec=c->ch+ends;c->ve=c->sec+SECTION_TERMS*sections;
    c->vc=(uint8_t *)(c->ve+4u*ends);
    bool ok=true;
    for(uint32_t i=0;i<ends&&ok;i++)ok=element(ctx,argv[0],i,&c->ch[i])&&isfinite(c->ch[i]);
    for(uint32_t i=0;i<sections&&ok;i++){
        JSValue row=JS_GetPropertyUint32(ctx,argv[1],i);
        uint32_t n=0;
        double *e=c->sec+SECTION_TERMS*i;
        ok=!JS_IsException(row)&&length_of(ctx,row,&n)&&(n==6||n==SECTION_TERMS);
        e[6]=0;
        for(uint32_t k=0;k<n&&ok;k++)ok=element(ctx,row,k,&e[k])&&isfinite(e[k]);
        /* A bend reads its heading (pose()). */
        if(ok&&e[1]!=0&&n!=SECTION_TERMS)ok=false;
        JS_FreeValue(ctx,row);
    }
    if(!ok){
        free(c);
        return fail(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"chord ends and section terms must be finite numbers");
    }
    free(course);
    course=c;
    view.set=false;
    return JS_UNDEFINED;
}
/* view(pc, zf, t, hl, crowd, hlColor, cells): the frame's camera. On the oval
 * it computes VE and VC as pan() did. */
static JSValue js_view(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *op="derby.view";
    if(!course)return fail(ctx,op,POCKET_ERR_BUSY,"course required");
    uint32_t n=0;
    bool ok=argc>=7&&length_of(ctx,argv[0],&n)&&n>=6;
    for(uint32_t i=0;i<6&&ok;i++)ok=element(ctx,argv[0],i,&view.pc[i]);
    ok=ok&&number(ctx,argv[1],&view.zf)&&number(ctx,argv[2],&view.t)&&
       plan_handle(ctx,argv[3],&view.hl)&&plan_handle(ctx,argv[4],&view.crowd)&&
       number(ctx,argv[5],&view.hl_color)&&number(ctx,argv[6],&view.cells);
    /* JS getters may have replaced the course while pc was read. */
    if(!ok||!course){
        view.set=false;
        return fail(ctx,op,!ok?POCKET_ERR_INVALID_ARGUMENT:POCKET_ERR_BUSY,
                    !ok?"expected camera, depth, frame, two plans and two numbers":"course required");
    }
    const double *pc=view.pc;
    course->in_view=0;
    if(course->sections){
        for(unsigned i=0;i<course->ends;i++){
            double m[4];
            pose(course->ch[i],0,m);
            const double x=m[0]-pc[0],z=m[1]-pc[1];
            double *V=course->ve+4u*i;
            V[0]=x*pc[3]-z*pc[2];V[1]=x*pc[2]+z*pc[3];V[2]=-m[3]*pc[3]-m[2]*pc[2];V[3]=m[2]*pc[3]-m[3]*pc[2];
        }
        for(unsigned j=1;j<course->ends;j++)
            if(!ou(j-1u,25.5,30,view.zf))course->vc[course->in_view++]=(uint8_t)(j-1u);
    }else course->vc[course->in_view++]=0;
    view.set=true;
    return JS_UNDEFINED;
}
/* ser(h, w, s, g0, L, a, b): ser(n, w, s, g0, L, zf, a, b) of derby_pan.js,
 * zf from view(). b omitted: the turf (t0, t1). */
static JSValue js_ser(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *op="derby.ser";
    uint32_t h;
    double w,s,g0,L,a,b=0;
    const bool turf=argc<7||JS_IsUndefined(argv[6]);
    if(argc<6||!plan_handle(ctx,argv[0],&h)||!number(ctx,argv[1],&w)||!number(ctx,argv[2],&s)||
       !number(ctx,argv[3],&g0)||!number(ctx,argv[4],&L)||!number(ctx,argv[5],&a)||
       (!turf&&!number(ctx,argv[6],&b)))
        return fail(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"expected plan, w, s, g0, L, a and optional b");
    if(!course||!view.set)return fail(ctx,op,POCKET_ERR_BUSY,"view required");
    const double *pc=view.pc,zf=view.zf;
    const bool ve=course->sections!=0;
    double P[4]={0},pg=0;
    bool have_pg=false;
    for(unsigned i=0;i<course->in_view;i++){
        const unsigned j=course->vc[i];
        if(ve&&ou(j,w,s,zf))continue;
        const double ga=g0+js_round((course->ch[j]-g0)/s)*s,gb=g0+js_round((course->ch[j+1]-g0)/s)*s;
        double A[4],B[4];
        if(have_pg&&ga==pg)memcpy(A,P,sizeof A);
        else pose(ga,w,A);
        pg=gb;have_pg=true;
        pose(pg,w,B);
        memcpy(P,B,sizeof P);
        const double dx=(B[0]-A[0])/(gb-ga),dz=(B[1]-A[1])/(gb-ga),X=A[0]+(g0-ga)*dx-pc[0],Y=A[1]+(g0-ga)*dz-pc[1],
            Q=(X*pc[2]+Y*pc[3])/pc[4],q[2]={X*pc[3]-Y*pc[2]+120*Q,Q},u=(dx*pc[2]+dz*pc[3])/pc[4],
            v=dx*pc[3]-dz*pc[2]+120*u;
        double Lo=ga-g0,Hi=gb-g0;
        lim(&Lo,&Hi,q[1]-.02,u);lim(&Lo,&Hi,zf/pc[4]-q[1],-u);lim(&Lo,&Hi,q[0]+40*q[1],v+40*u);
        lim(&Lo,&Hi,280*q[1]-q[0],280*u-v);
        if(!(Lo<Hi))continue;
        double c0=a,c1=b;
        if(turf){
            const double l=a/sqrt(dx*dx+dz*dz),x=-dz*l,z=dx*l;
            c1=-(x*pc[2]+z*pc[3])/pc[4];c0=x*pc[3]-z*pc[2]-120*c1;
        }
        const double o=u<0?-1:1,U=u*o,V=v*o,t1=o>0?Hi:-Lo;
        double k=s,Z=sqrt(fabs(v*q[1]-q[0]*u)*s/L),n0=floor((o>0?Lo:-Hi)/s)*s,e;
        while(Z<q[1]+t1*U&&2*k*U<.3*(q[1]+t1*U)&&k<64*s)k*=2,Z*=M_SQRT2;
        e=ceil(t1/k)*k;
        if(!inr(q,U,V,e))e-=k;
        if(!inr(q,U,V,n0))n0+=s;
        if(!(L!=0&&!isnan(L))&&(js_int32(js_round(o*e/s)+view.t)&1))e-=s;
        for(;;){
            Z/=M_SQRT2;
            const double hh=k>s?ceil(js_max(U>1e-7?(js_max(Z,k*U/.3)-q[1])/U:-1e9,n0)/k)*k:n0,
                c=js_round((e-hh)/k),z=q[1]+e*U;
            if(c>0){
                const double in[8]={q[0]+e*V,-V*k,-z,U*k,js_min(255,c),1/z,
                    (L!=0&&!isnan(L))?c0:fmod(-e/s*2.39996,2*M_PI)-2*M_PI*js_round(c*.191)+1.8,c1};
                JSValue r=draw(ctx,h,in);
                if(JS_IsException(r))return r;
            }
            if(k==s)break;
            e=js_min(e,hh);k/=2;
        }
        if(!turf&&b<0){
            const double y=q[1]+Lo*u,z=q[1]+Hi*u,xa=(q[0]+Lo*v)/y,xb=(q[0]+Hi*v)/z;
            const double in_hl[8]={xa,1/y,xb,1/z,6,-2.4,view.hl_color,21130},
                in_crowd[8]={xa,28+4.8/y,xb,28+4.8/z,-2.4/y,-2.4/z,Lo*view.cells,Hi*view.cells};
            JSValue r=draw(ctx,view.hl,in_hl);
            if(JS_IsException(r))return r;
            r=draw(ctx,view.crowd,in_crowd);
            if(JS_IsException(r))return r;
        }
        /* A draw's host hook (harness only) may call course(). */
        if(!course)break;
    }
    return JS_UNDEFINED;
}

esp_err_t pocket_derby_fill(JSContext *ctx,JSValueConst ns,void *unused){
    (void)unused;
    static const JSCFunctionListEntry methods[]={
        JS_CFUNC_DEF("course",3,js_course),
        JS_CFUNC_DEF("view",7,js_view),
        JS_CFUNC_DEF("ser",7,js_ser),
    };
    return JS_SetPropertyFunctionList(ctx,ns,methods,(int)(sizeof methods/sizeof methods[0]))<0?
           ESP_ERR_NO_MEM:ESP_OK;
}
/* Every number here is refused past by js_course(). */
static const pocket_limit_t derby_limits[]={
    {.name="maxEnds",.kind=POCKET_LIMIT_INT,.number=DERBY_ENDS_MAX},
    {.name="maxSections",.kind=POCKET_LIMIT_INT,.number=DERBY_SECTIONS_MAX},
    {0},
};
static const pocket_capability_t capability={.name="derby.series",.supported=true,
    .available=true,.limits=derby_limits};
esp_err_t pocket_derby_install(JSContext *ctx,void *unused){
    (void)unused;
    esp_err_t err=pocket_api_register(&capability);
    return err==ESP_OK?pocket_api_lazy(ctx,"derby",pocket_derby_fill,NULL):err;
}
void pocket_derby_reset(void){
    free(course);
    course=NULL;
    memset(&view,0,sizeof view);
}
