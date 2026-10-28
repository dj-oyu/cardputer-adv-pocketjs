#include "pocket_shape.h"
#include "pocket_api.h"
#include "pocket_proc.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* WAKE's scan conversion and projection (apps/kasane/megademo_wake.js at
 * dd3bfba: wspan, wminus, wstrips, winside, tf) moved to C, operation for
 * operation in the same order, so a build with POCKET_SHAPE_DOUBLE (the host
 * oracle, tools/kasane_contract/run_shape_oracle.py) issues the very draws
 * the JS did, bit for bit. The device build computes in float: the S3 has a
 * float unit but does doubles in software, and the scan's ~5,000 edge tests a
 * frame in soft double would cost ~10 ms (estimate) of the turn this exists
 * to save; float moves an endpoint by under a pixel at these sizes
 * (docs/apps/megademo-shapes.md). The build keeps the compiler from fusing
 * (-ffp-contract=off, main/CMakeLists.txt). */
#ifdef POCKET_SHAPE_DOUBLE
typedef double real;
#define R_SQRT sqrt
#define R_CEIL ceil
#define R_FLOOR floor
#else
typedef float real;
#define R_SQRT sqrtf
#define R_CEIL ceilf
#define R_FLOOR floorf
#endif

#define SHAPE_HOLES 6u      /* holes a fill subtracts */
#define SHAPE_POINTS 512u   /* points a buffer may address */
#define SHAPE_GAPS (2u*(SHAPE_HOLES+1u))

typedef struct { const double *p; unsigned n; } poly;

static JSValue fail(JSContext *ctx,const char *op,const char *code,const char *message){
    return pocket_api_throw(ctx,code,op,message,false,POCKET_OUTCOME_NOT_APPLIED);
}
static bool number(JSContext *ctx,JSValueConst v,double *out){
    return JS_IsNumber(v)&&JS_ToFloat64(ctx,out,v)==0;
}
/* A plan handle as the frame loop's DRAW reads live[k]: falsy draws nothing,
 * else a handle-shaped integer (pocket_proc_draw_numbers looks it up). */
static bool plan_handle(JSContext *ctx,JSValueConst v,uint32_t *out){
    double n;
    *out=0;
    if(!JS_ToBool(ctx,v))return true;
    if(!number(ctx,v,&n)||n<1||n>INT32_MAX||n!=floor(n))return false;
    *out=(uint32_t)n;return true;
}
/* Up to max integers 0..SHAPE_POINTS from a plain array. */
static bool integers(JSContext *ctx,JSValueConst array,uint32_t *out,unsigned max,unsigned *count){
    double n;
    if(!JS_IsArray(array))return false;
    JSValue v=JS_GetPropertyStr(ctx,array,"length");
    bool ok=!JS_IsException(v)&&number(ctx,v,&n)&&n>=0&&n<=max&&n==floor(n);
    JS_FreeValue(ctx,v);
    if(!ok)return false;
    *count=(unsigned)n;
    for(unsigned i=0;i<*count;i++){
        v=JS_GetPropertyUint32(ctx,array,i);
        ok=!JS_IsException(v)&&number(ctx,v,&n)&&n>=0&&n<=SHAPE_POINTS&&n==floor(n);
        JS_FreeValue(ctx,v);
        if(!ok)return false;
        out[i]=(uint32_t)n;
    }
    return true;
}
/* A Float64Array's elements, borrowed: nothing may run JS between this and
 * the last use (every argument is read before). */
static double *doubles(JSContext *ctx,JSValueConst v,unsigned *count){
    if(JS_GetTypedArrayType(v)!=JS_TYPED_ARRAY_FLOAT64)return NULL;
    size_t offset=0,bytes=0,per=0,size=0;
    JSValue buffer=JS_GetTypedArrayBuffer(ctx,v,&offset,&bytes,&per);
    if(JS_IsException(buffer))return NULL;
    uint8_t *data=JS_GetArrayBuffer(ctx,&size,buffer);
    const bool mutable_=JS_IsImmutableArrayBuffer(buffer)<=0;
    JS_FreeValue(ctx,buffer);
    if(!data||!mutable_||per!=sizeof(double)||offset>size||bytes>size-offset)return NULL;
    *count=(unsigned)(bytes/sizeof(double));
    return (double *)(data+offset);
}
/* Polygons named by [start, count] pairs of a points buffer (2 numbers a
 * point). */
static bool polys(const uint32_t *pairs,unsigned n,const double *points,unsigned numbers,poly *out){
    for(unsigned i=0;i<n;i+=2){
        if(pairs[i+1]<3u||2u*(pairs[i]+pairs[i+1])>numbers)return false;
        out[i/2]=(poly){points+2u*pairs[i],pairs[i+1]};
    }
    return true;
}

/* ---- wspan: the x span of a convex polygon on row y; false if none. */
static bool span(const poly *q,real y,real *l,real *r){
    real lo=(real)1e9,hi=(real)-1e9;
    for(unsigned k=0;k<q->n;k++){
        const double *a=q->p+2u*k,*b=q->p+2u*((k+1u)%q->n);
        const real a0=(real)a[0],a1=(real)a[1],b0=(real)b[0],b1=(real)b[1];
        if((a1-y)*(b1-y)<=0&&a1!=b1){
            const real x=a0+(b0-a0)*(y-a1)/(b1-a1);
            if(x<lo)lo=x;
            if(x>hi)hi=x;
        }
    }
    *l=lo;*r=hi;
    return lo<=hi;
}
/* ---- wminus: row y of [l, r] (clipped to the screen) less the holes'
 * spans, as x0, x1 pairs into g; the pair count. */
static unsigned minus(real l,real r,const poly *holes,unsigned nh,real y,real *g){
    real hs[SHAPE_HOLES][2];
    unsigned n=0;
    for(unsigned k=0;k<nh;k++){
        real a,b;
        if(!span(&holes[k],y,&a,&b))continue;
        unsigned j=n++;
        while(j&&hs[j-1][0]>a){hs[j][0]=hs[j-1][0];hs[j][1]=hs[j-1][1];--j;}
        hs[j][0]=a;hs[j][1]=b;
    }
    unsigned m=0;
    real x=l<0?0:l;
    const real e=r>239?239:r;
    for(unsigned k=0;k<n;k++){
        if(hs[k][0]>x){g[m++]=x;g[m++]=hs[k][0]<e?hs[k][0]:e;}
        if(hs[k][1]>x)x=hs[k][1];
    }
    if(x<e){g[m++]=x;g[m++]=e;}
    unsigned kept=0;
    for(unsigned k=0;k<m;k+=2)if(g[k]<g[k+1]){g[kept++]=g[k];g[kept++]=g[k+1];}
    return kept/2;
}
/* What a fill covers on a row: a polygon (rx < 0) or an ellipse. */
typedef struct {
    poly base;
    real cx,cy,rx,ry;
    const poly *holes;
    unsigned nh;
} region;
static unsigned rows(const region *s,real y,real *g){
    if(s->rx<0){
        real l,r;
        return span(&s->base,y,&l,&r)?minus(l,r,s->holes,s->nh,y,g):0;
    }
    const real v=(y-s->cy)/s->ry,q=1-v*v,w=s->rx*R_SQRT(q>0?q:0);
    return w>0?minus(s->cx-w,s->cx+w,s->holes,s->nh,y,g):0;
}
static JSValue trapezoid(JSContext *ctx,uint32_t h,real x0,real x1,real y,real d0,real d1,real n){
    const double in[6]={x0,x1,y,d0,d1,n};
    return h?pocket_proc_draw_numbers(ctx,h,in,6):JS_UNDEFINED;
}
/* ---- wstrips: rows y0..y1 in strips of 4 rows, one trapezoid a gap; where
 * the gap count changes in a strip, row by row. A line every s rows. */
static JSValue strips(JSContext *ctx,const region *s,real y0,real y1,uint32_t h,unsigned step){
    real a[SHAPE_GAPS],b[SHAPE_GAPS],c[SHAPE_GAPS];
    const real st=(real)step;
    y0=R_CEIL(y0);if(y0<0)y0=0;
    y1=R_FLOOR(y1);if(y1>134)y1=134;
    if(step>1)y0+=(real)((int32_t)y0&1);
    for(real y=y0;y<y1;y+=4){
        const real n=y1-y<4?y1-y:4;
        const unsigned na=rows(s,y,a),nb=rows(s,y+n,b);
        const real m=n/st;
        if(n<st)break;
        if(na==nb){
            for(unsigned j=0;j<2u*na;j+=2){
                JSValue r=trapezoid(ctx,h,a[j],a[j+1],y,(b[j]-a[j])/m,(b[j+1]-a[j+1])/m,m);
                if(JS_IsException(r))return r;
            }
        }else for(real k=0;k<n;k+=st){
            const unsigned nc=rows(s,y+k,c);
            for(unsigned j=0;j<2u*nc;j+=2){
                JSValue r=trapezoid(ctx,h,c[j],c[j+1],y+k,0,0,1);
                if(JS_IsException(r))return r;
            }
        }
    }
    return JS_UNDEFINED;
}

/* project(model, points, from, count, pose): model points (x, y, z: x right,
 * y down, z to the viewer) from..from+count-1 to screen points (x, y) at the
 * same index. pose = [cos yaw, sin yaw, cos pitch, sin pitch, cos roll,
 * sin roll, camera distance, scale, x0, y0]: turn, nod, perspective (nearer
 * is bigger), roll. */
static JSValue js_project(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *op="shapes.project";
    double from,count;
    if(argc!=5||!number(ctx,argv[2],&from)||!number(ctx,argv[3],&count)||from<0||count<0||
       from!=floor(from)||count!=floor(count)||from+count>SHAPE_POINTS)
        return fail(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"expected model, points, from, count, pose");
    unsigned nm,np,npose;
    const double *m=doubles(ctx,argv[0],&nm),*pose=doubles(ctx,argv[4],&npose);
    double *p=doubles(ctx,argv[1],&np);
    const unsigned i0=(unsigned)from,n=(unsigned)count;
    if(!m||!p||!pose||npose<10||3u*(i0+n)>nm||2u*(i0+n)>np)
        return fail(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"Float64Array model, points and pose too short");
    const real cy=(real)pose[0],sy=(real)pose[1],cp=(real)pose[2],sp=(real)pose[3],
        c=(real)pose[4],sn=(real)pose[5],D=(real)pose[6],S=(real)pose[7],X0=(real)pose[8],Y0=(real)pose[9];
    for(unsigned i=i0;i<i0+n;i++){
        const real x=(real)m[3*i],y=(real)m[3*i+1],z=(real)m[3*i+2];
        const real x1=x*cy+z*sy,z1=z*cy-x*sy,y2=y*cp-z1*sp,z2=y*sp+z1*cp,k=D/(D-z2)*S;
        p[2*i]=X0+(x1*c-y2*sn)*k;
        p[2*i+1]=Y0+(x1*sn+y2*c)*k;
    }
    return JS_UNDEFINED;
}
/* fill(points, plan, step, y0, y1, base, holes): rows y0..y1 (null: the
 * polygon's own, inset by .01) of base less the holes, drawn with plan (the
 * trapezoid plan: x0, x1, y, dx0, dx1, rows) every step rows. base is
 * [start, count] (a convex polygon of the buffer) or [cx, cy, rx, ry] (an
 * ellipse); holes are [start, count, ...] pairs, at most 6. */
static JSValue js_fill(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *op="shapes.fill";
    uint32_t h,hp[2*SHAPE_HOLES],bp[4];
    unsigned nhp=0,nb=0;
    double step,y0=0,y1=0,e[4];
    const bool own=argc==7&&JS_IsNull(argv[3])&&JS_IsNull(argv[4]);
    bool ok=argc==7&&plan_handle(ctx,argv[1],&h)&&number(ctx,argv[2],&step)&&(step==1||step==2)&&
        (own||(number(ctx,argv[3],&y0)&&number(ctx,argv[4],&y1)&&isfinite(y0)&&isfinite(y1)))&&
        integers(ctx,argv[6],hp,2*SHAPE_HOLES,&nhp)&&!(nhp&1u);
    if(ok&&JS_IsArray(argv[5])){
        JSValue v=JS_GetPropertyStr(ctx,argv[5],"length");
        double len;
        ok=!JS_IsException(v)&&number(ctx,v,&len)&&(len==2||len==4);
        JS_FreeValue(ctx,v);
        nb=ok?(unsigned)len:0;
        for(unsigned i=0;ok&&i<nb;i++){
            v=JS_GetPropertyUint32(ctx,argv[5],i);
            ok=!JS_IsException(v)&&number(ctx,v,&e[i])&&isfinite(e[i]);
            JS_FreeValue(ctx,v);
        }
        if(ok&&nb==2){
            ok=e[0]>=0&&e[1]>=3&&e[0]+e[1]<=SHAPE_POINTS&&e[0]==floor(e[0])&&e[1]==floor(e[1]);
            bp[0]=(uint32_t)e[0];bp[1]=(uint32_t)e[1];
        }
        if(ok&&nb==4)ok=e[2]>0&&e[3]>0&&!own;
    }else ok=false;
    if(!ok)return fail(ctx,op,POCKET_ERR_INVALID_ARGUMENT,
                       "expected points, plan, step 1|2, y0, y1, [start, count]|[cx, cy, rx, ry], holes");
    unsigned np;
    const double *p=doubles(ctx,argv[0],&np);
    poly holes[SHAPE_HOLES];
    region s={.rx=-1,.holes=holes,.nh=nhp/2};
    if(!p||!polys(hp,nhp,p,np,holes)||(nb==2&&!polys(bp,2,p,np,&s.base)))
        return fail(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"Float64Array points too short");
    real lo=(real)y0,hi=(real)y1;
    if(nb==4){s.cx=(real)e[0];s.cy=(real)e[1];s.rx=(real)e[2];s.ry=(real)e[3];}
    else if(own){
        lo=(real)1e9;hi=(real)-1e9;
        for(unsigned k=0;k<s.base.n;k++){
            const real y=(real)s.base.p[2*k+1];
            if(y<lo)lo=y;
            if(y>hi)hi=y;
        }
        lo+=(real).01;hi-=(real).01;
    }
    return strips(ctx,&s,lo,hi,h,(unsigned)step);
}
/* winside: inside a convex polygon of either winding (on an edge counts). */
static bool inside(const poly *q,real px,real py){
    unsigned pos=0,neg=0;
    for(unsigned k=0;k<q->n;k++){
        const double *a=q->p+2u*k,*b=q->p+2u*((k+1u)%q->n);
        const real x=((real)b[0]-(real)a[0])*(py-(real)a[1])-((real)b[1]-(real)a[1])*(px-(real)a[0]);
        if(x>0)++pos;else if(x<0)++neg;
    }
    return !pos||!neg;
}
static JSValue line(JSContext *ctx,uint32_t h,real ax,real ay,real bx,real by){
    if((ax>239&&bx>239)||(ay<0&&by<0)||(ax<0&&bx<0)||(ay>134&&by>134))return JS_UNDEFINED;
    const double in[4]={ax,ay,bx,by};
    return h?pocket_proc_draw_numbers(ctx,h,in,4):JS_UNDEFINED;
}
/* outline(points, plan, start, count, closed, parts, hidden): the edges of
 * points start..start+count-1 (and back to the first when closed), each in
 * `parts` pieces, drawn with plan (a line: x0, y0, x1, y1) unless the piece's
 * middle is inside one of the hidden polygons ([start, count, ...] pairs).
 * Pieces wholly off one side of the screen are left out. */
static JSValue js_outline(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *op="shapes.outline";
    uint32_t h,hp[2*SHAPE_HOLES];
    unsigned nhp=0;
    double from,count,parts;
    if(argc!=7||!plan_handle(ctx,argv[1],&h)||!number(ctx,argv[2],&from)||!number(ctx,argv[3],&count)||
       !number(ctx,argv[5],&parts)||parts<1||parts>8||parts!=floor(parts)||
       from<0||count<2||from!=floor(from)||count!=floor(count)||from+count>SHAPE_POINTS||
       !integers(ctx,argv[6],hp,2*SHAPE_HOLES,&nhp)||(nhp&1u))
        return fail(ctx,op,POCKET_ERR_INVALID_ARGUMENT,
                    "expected points, plan, start, count, closed, parts 1..8, hidden");
    const bool closed=JS_ToBool(ctx,argv[4]);
    unsigned np;
    const double *p=doubles(ctx,argv[0],&np);
    poly hidden[SHAPE_HOLES];
    const unsigned i0=(unsigned)from,n=(unsigned)count,k=(unsigned)parts;
    if(!p||2u*(i0+n)>np||!polys(hp,nhp,p,np,hidden))
        return fail(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"Float64Array points too short");
    for(unsigned e=0;e<(closed?n:n-1u);e++){
        const double *a=p+2u*(i0+e),*b=p+2u*(i0+(e+1u)%n);
        const real a0=(real)a[0],a1=(real)a[1],b0=(real)b[0],b1=(real)b[1];
        for(unsigned j=0;j<k;j++){
            /* One piece is the edge itself: a + (b - a) need not be b. */
            const real p0=k==1?a0:a0+(b0-a0)*(real)j/(real)k,p1=k==1?a1:a1+(b1-a1)*(real)j/(real)k,
                r0=k==1?b0:a0+(b0-a0)*(real)(j+1)/(real)k,r1=k==1?b1:a1+(b1-a1)*(real)(j+1)/(real)k;
            const real mx=(p0+r0)/2,my=(p1+r1)/2;
            bool hide=false;
            for(unsigned q=0;q<nhp/2&&!hide;q++)hide=inside(&hidden[q],mx,my);
            if(hide)continue;
            JSValue r=line(ctx,h,p0,p1,r0,r1);
            if(JS_IsException(r))return r;
        }
    }
    return JS_UNDEFINED;
}

esp_err_t pocket_shape_fill(JSContext *ctx,JSValueConst ns,void *unused){
    (void)unused;
    static const JSCFunctionListEntry methods[]={
        JS_CFUNC_DEF("project",5,js_project),
        JS_CFUNC_DEF("fill",7,js_fill),
        JS_CFUNC_DEF("outline",7,js_outline),
    };
    return JS_SetPropertyFunctionList(ctx,ns,methods,(int)(sizeof methods/sizeof methods[0]))<0?
           ESP_ERR_NO_MEM:ESP_OK;
}
/* Every number here is refused past by the functions. */
static const pocket_limit_t shape_limits[]={
    {.name="maxPoints",.kind=POCKET_LIMIT_INT,.number=SHAPE_POINTS},
    {.name="maxHoles",.kind=POCKET_LIMIT_INT,.number=SHAPE_HOLES},
    {0},
};
static const pocket_capability_t capability={.name="kasane.shapes",.supported=true,
    .available=true,.limits=shape_limits};
esp_err_t pocket_shape_install(JSContext *ctx,void *unused){
    (void)unused;
    esp_err_t err=pocket_api_register(&capability);
    return err==ESP_OK?pocket_api_lazy(ctx,"shapes",pocket_shape_fill,NULL):err;
}
