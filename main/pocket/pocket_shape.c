#include "pocket_shape.h"
#include "pocket_api.h"
#include "pocket_proc.h"
#include "ayatsuri/ayatsuri_transform.h"
#include "ayatsuri/ayatsuri_follow.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* pocket.shapes: flat vector drawing into the procedural frame being built,
 * in C (docs/api/shapes.md is the contract; docs/apps/megademo-shapes.md the
 * reason: MEGADEMO's WAKE spent ~130 ms a frame of JS on this geometry).
 * Points live in the caller's Float32Array (float is what the geometry is
 * computed in; a Float64Array would only double the memory) and are named
 * by index, so a frame allocates nothing. fill() and outline() write segments
 * straight into the candidate frame (pocket_proc_native_*): no plan, no VM,
 * the frame's own limits. Everything is computed in float -- the S3 has a
 * float unit and does doubles in software -- and the build keeps the
 * compiler from fusing (-ffp-contract=off) so host and device agree. */

#define SHAPE_POINTS 1024u  /* points a buffer may address */
#define SHAPE_POLY 256u     /* vertices of one polygon */
#define SHAPE_HOLES 6u      /* polygons a fill subtracts or an outline hides behind */
#define SHAPE_W 240
#define SHAPE_H 135

typedef struct {
    float *data;
    unsigned points;  /* whole points the buffer holds */
} buffer;
typedef struct { const buffer *b; unsigned start,count; bool empty; } poly;

static JSValue fail(JSContext *ctx,const char *op,const char *code,const char *message){
    return pocket_api_throw(ctx,code,op,message,false,POCKET_OUTCOME_NOT_APPLIED);
}
static JSValue invalid(JSContext *ctx,const char *op,const char *message){
    return fail(ctx,op,POCKET_ERR_INVALID_ARGUMENT,message);
}
static bool number(JSContext *ctx,JSValueConst v,double *out){
    return JS_IsNumber(v)&&JS_ToFloat64(ctx,out,v)==0;
}
static bool whole(double n,double low,double high){return n>=low&&n<=high&&n==floor(n);}
/* A Float32Array as `stride` numbers a point, borrowed: nothing may run JS
 * between this and the last use, so every other argument is read before.
 * Mutable: project() writes through it. */
static bool view(JSContext *ctx,JSValueConst v,unsigned stride,buffer *out){
    if(JS_GetTypedArrayType(v)!=JS_TYPED_ARRAY_FLOAT32)return false;
    size_t offset=0,bytes=0,per=0,size=0;
    JSValue ab=JS_GetTypedArrayBuffer(ctx,v,&offset,&bytes,&per);
    if(JS_IsException(ab))return false;
    uint8_t *data=JS_GetArrayBuffer(ctx,&size,ab);
    const bool immutable=JS_IsImmutableArrayBuffer(ab)>0;
    JS_FreeValue(ctx,ab);
    if(!data||immutable||offset>size||bytes>size-offset)return false;
    out->data=(float *)(data+offset);
    out->points=(unsigned)(bytes/per/stride);
    return true;
}
static inline float get(const buffer *b,unsigned i){return b->data[i];}
static inline void set(const buffer *b,unsigned i,float v){b->data[i]=v;}
static inline float X(const poly *q,unsigned k){return get(q->b,2u*(q->start+k%q->count));}
static inline float Y(const poly *q,unsigned k){return get(q->b,2u*(q->start+k%q->count)+1u);}
/* [start, count, start, count, ...] from a plain array: pairs naming
 * polygons of 3..SHAPE_POLY points, at most `max` of them. */
static bool pairs(JSContext *ctx,JSValueConst array,uint32_t *out,unsigned max,unsigned *n){
    double len,v;
    *n=0;
    if(JS_IsUndefined(array))return true;
    if(!JS_IsArray(array))return false;
    JSValue l=JS_GetPropertyStr(ctx,array,"length");
    bool ok=!JS_IsException(l)&&number(ctx,l,&len)&&whole(len,0,2*max)&&!((unsigned)len&1u);
    JS_FreeValue(ctx,l);
    for(unsigned i=0;ok&&i<(unsigned)len;i++){
        JSValue e=JS_GetPropertyUint32(ctx,array,i);
        ok=!JS_IsException(e)&&number(ctx,e,&v)&&
           (i&1u?whole(v,3,SHAPE_POLY):whole(v,0,SHAPE_POINTS-1));
        JS_FreeValue(ctx,e);
        out[i]=(uint32_t)v;
    }
    if(ok)*n=(unsigned)len/2u;
    return ok;
}
/* Polygons from pairs over the points buffer; one with a non-finite point is
 * empty (a point behind the camera, project()). */
static bool polys(const uint32_t *pp,unsigned n,const buffer *b,poly *out){
    for(unsigned i=0;i<n;i++){
        poly *q=&out[i];
        *q=(poly){b,pp[2*i],pp[2*i+1],false};
        if(q->start+q->count>b->points)return false;
        for(unsigned k=0;k<q->count&&!q->empty;k++)q->empty=!isfinite(X(q,k))||!isfinite(Y(q,k));
    }
    return true;
}
/* An options object's numeric field: absent (undefined) keeps *out. */
static bool option(JSContext *ctx,JSValueConst options,const char *name,double *out,bool *given){
    if(given)*given=false;
    if(JS_IsUndefined(options))return true;
    JSValue v=JS_GetPropertyStr(ctx,options,name);
    if(JS_IsException(v))return false;
    bool ok=true;
    if(!JS_IsUndefined(v)){
        ok=JS_IsBool(v)?(*out=JS_ToBool(ctx,v),true):number(ctx,v,out);
        if(given)*given=ok;
    }
    JS_FreeValue(ctx,v);
    return ok;
}

/* ---- project(matrix, source, target[, from[, count]]) */
static JSValue js_shape_project(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *op="shapes.project";
    double m[12],from=0,count=-1;
    if(argc<3||argc>5)return invalid(ctx,op,"expected matrix, source, target[, from[, count]]");
    if((argc>3&&!(number(ctx,argv[3],&from)&&whole(from,0,SHAPE_POINTS)))||
       (argc>4&&!(number(ctx,argv[4],&count)&&whole(count,0,SHAPE_POINTS))))
        return invalid(ctx,op,"from and count must be integers 0..1024");
    /* The matrix: 12 finite numbers, a plain array or a Float32Array. */
    if(!JS_IsArray(argv[0])&&JS_GetTypedArrayType(argv[0])!=JS_TYPED_ARRAY_FLOAT32)
        return invalid(ctx,op,"matrix must be 12 numbers");
    for(unsigned i=0;i<12;i++){
        JSValue v=JS_GetPropertyUint32(ctx,argv[0],i);
        const bool ok=!JS_IsException(v)&&number(ctx,v,&m[i])&&isfinite(m[i]);
        JS_FreeValue(ctx,v);
        if(!ok)return invalid(ctx,op,"matrix must be 12 finite numbers");
    }
    buffer src,dst;
    if(!view(ctx,argv[1],3,&src)||!view(ctx,argv[2],2,&dst))
        return invalid(ctx,op,"source and target must be mutable Float32Arrays");
    const unsigned i0=(unsigned)from,n=count<0?(i0<src.points?src.points-i0:0):(unsigned)count;
    if(i0+n>src.points||i0+n>dst.points||i0+n>SHAPE_POINTS)
        return fail(ctx,op,POCKET_ERR_LIMIT_EXCEEDED,"points past the end of source or target");
    float a[12];
    for(unsigned i=0;i<12;i++)a[i]=(float)m[i];
    for(unsigned i=i0;i<i0+n;i++){
        const float x=get(&src,3*i),y=get(&src,3*i+1),z=get(&src,3*i+2);
        const float w=a[8]*x+a[9]*y+a[10]*z+a[11];
        if(!(w>0)){set(&dst,2*i,NAN);set(&dst,2*i+1,NAN);continue;}
        set(&dst,2*i,(a[0]*x+a[1]*y+a[2]*z+a[3])/w);
        set(&dst,2*i+1,(a[4]*x+a[5]*y+a[6]*z+a[7])/w);
    }
    return JS_UNDEFINED;
}

/* shade() separates body-attached albedo from world-space lighting. These
 * borrowed views last only for this synchronous call; no JS is run after
 * validation and no buffer or callback is retained. */
typedef struct { uint8_t *data;size_t bytes; } shade_view;
static bool shade_buffer(JSContext *ctx,JSValueConst value,JSTypedArrayEnum type,
                         bool writable,shade_view *out){
    if(JS_GetTypedArrayType(value)!=(int)type)return false;
    size_t offset=0,bytes=0,per=0,size=0;
    JSValue ab=JS_GetTypedArrayBuffer(ctx,value,&offset,&bytes,&per);
    if(JS_IsException(ab))return false;
    uint8_t *raw=JS_GetArrayBuffer(ctx,&size,ab);
    const bool immutable=JS_IsImmutableArrayBuffer(ab)>0;
    bool ok=raw&&offset<=size&&bytes<=size-offset&&(!writable||!immutable);
    if(ok)*out=(shade_view){raw+offset,bytes};
    JS_FreeValue(ctx,ab);
    return ok;
}
static bool shade_overlap(const shade_view *a,const shade_view *b){
    const uintptr_t x=(uintptr_t)a->data,y=(uintptr_t)b->data;
    return a->bytes&&b->bytes&&(x<=y?y-x<a->bytes:x-y<b->bytes);
}
/* Ayatsuri owns evaluation semantics; hone still owns drawing. Keep this
 * small bridge beside the existing typed-view checks, without linking a
 * second implementation or retaining guest buffers after this call. */
#include "pocket_ayatsuri_follow.inc"
static JSValue js_ayatsuri_compose(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;const char *op="ayatsuri.compose";
    if(argc!=3)return invalid(ctx,op,"expected Float32 pose[9], parent[12] or null, output[12]");
    shade_view pose_view,parent_view,output_view;
    const bool has_parent=!JS_IsNull(argv[1])&&!JS_IsUndefined(argv[1]);
    if(!shade_buffer(ctx,argv[0],JS_TYPED_ARRAY_FLOAT32,false,&pose_view)||pose_view.bytes!=36||
       !shade_buffer(ctx,argv[2],JS_TYPED_ARRAY_FLOAT32,true,&output_view)||output_view.bytes!=48||
       (has_parent&&(!shade_buffer(ctx,argv[1],JS_TYPED_ARRAY_FLOAT32,false,&parent_view)||parent_view.bytes!=48)))
        return invalid(ctx,op,"expected Float32 pose[9], parent[12] or null, writable output[12]");
    ayatsuri_pose pose;ayatsuri_transform parent,result;
    memcpy(&pose,pose_view.data,sizeof pose);
    if(has_parent)memcpy(&parent,parent_view.data,sizeof parent);
    const ayatsuri_status status=ayatsuri_transform_evaluate(&pose,has_parent?&parent:NULL,&result);
    if(status==AYATSURI_INVALID)return invalid(ctx,op,"finite pose/parent and nonzero scale required");
    if(status!=AYATSURI_OK)return fail(ctx,op,POCKET_ERR_LIMIT_EXCEEDED,"float32 transform overflow");
    memcpy(output_view.data,&result,sizeof result);
    return JS_UNDEFINED;
}
/* Utility-private packed node: matrix[12], pose[9]. One stable view replaces
 * two ArrayBuffer view headers per object without changing scalar semantics. */
static JSValue js_ayatsuri_compose_node(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;const char *op="ayatsuri.composeNode";shade_view node,parent;
    const bool has_parent=argc==2&&!JS_IsNull(argv[1])&&!JS_IsUndefined(argv[1]);
    if(argc!=2||!shade_buffer(ctx,argv[0],JS_TYPED_ARRAY_FLOAT32,true,&node)||node.bytes!=84||
       (has_parent&&(!shade_buffer(ctx,argv[1],JS_TYPED_ARRAY_FLOAT32,false,&parent)||(parent.bytes!=84&&parent.bytes!=48))))
        return invalid(ctx,op,"expected writable node Float32[21], parent Float32[21/12] or null");
    ayatsuri_pose pose;ayatsuri_transform p,result;
    memcpy(&pose,node.data+48,sizeof pose);
    if(has_parent)memcpy(&p,parent.data,sizeof p);
    const ayatsuri_status status=ayatsuri_transform_evaluate(&pose,has_parent?&p:NULL,&result);
    if(status==AYATSURI_INVALID)return invalid(ctx,op,"invalid node pose/parent");
    if(status!=AYATSURI_OK)return fail(ctx,op,POCKET_ERR_LIMIT_EXCEEDED,"float32 transform overflow");
    memcpy(node.data,&result,sizeof result);return JS_UNDEFINED;
}
/* All packets are synchronously borrowed. Stack snapshots make aliasing safe;
 * only a fully validated result is copied back into guest-owned storage. */
static JSValue js_ayatsuri_follow_prepare(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;const char *op="ayatsuri.followPrepare";shade_view input,output;
    if(argc!=2||!shade_buffer(ctx,argv[0],JS_TYPED_ARRAY_FLOAT32,false,&input)||input.bytes!=32||
       !shade_buffer(ctx,argv[1],JS_TYPED_ARRAY_FLOAT64,true,&output)||output.bytes!=112)
        return invalid(ctx,op,"expected config Float32[8], writable prepared Float64[14]");
    ayatsuri_follow_config config;ayatsuri_follow_coeff prepared;
    memcpy(&config,input.data,sizeof config);
    const ayatsuri_status status=ayatsuri_follow_prepare(&config,&prepared);
    if(status==AYATSURI_RANGE)return fail(ctx,op,POCKET_ERR_LIMIT_EXCEEDED,"follow coefficient range");
    if(status!=AYATSURI_OK)return invalid(ctx,op,"invalid follow coefficients");
    memcpy(output.data,&prepared,sizeof prepared);return JS_UNDEFINED;
}
static JSValue js_ayatsuri_follow_step(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;const char *op="ayatsuri.followStep";shade_view coeff,state,goal,force;
    if(argc!=4||!shade_buffer(ctx,argv[0],JS_TYPED_ARRAY_FLOAT64,false,&coeff)||coeff.bytes!=112||
       !shade_buffer(ctx,argv[1],JS_TYPED_ARRAY_FLOAT32,true,&state)||state.bytes!=16||
       !shade_buffer(ctx,argv[2],JS_TYPED_ARRAY_FLOAT32,false,&goal)||goal.bytes!=8||
       !shade_buffer(ctx,argv[3],JS_TYPED_ARRAY_FLOAT32,false,&force)||force.bytes!=8)
        return invalid(ctx,op,"expected prepared Float64[14], state Float32[4], goal/force Float32[2]");
    ayatsuri_follow_coeff c;ayatsuri_follow_state s;ayatsuri_follow_goal g;float f[2];
    memcpy(&c,coeff.data,sizeof c);memcpy(&s,state.data,sizeof s);memcpy(&g,goal.data,sizeof g);memcpy(f,force.data,sizeof f);
    const ayatsuri_status status=ayatsuri_follow_step(&c,&g,f,&s);
    if(status==AYATSURI_RANGE)return fail(ctx,op,POCKET_ERR_LIMIT_EXCEEDED,"float32 follow overflow");
    if(status!=AYATSURI_OK)return invalid(ctx,op,"invalid follow step");
    memcpy(state.data,&s,sizeof s);return JS_UNDEFINED;
}
static JSValue js_ayatsuri_follow_impulse(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;const char *op="ayatsuri.followImpulse";shade_view coeff,state,impulse;
    if(argc!=3||!shade_buffer(ctx,argv[0],JS_TYPED_ARRAY_FLOAT64,false,&coeff)||coeff.bytes!=112||
       !shade_buffer(ctx,argv[1],JS_TYPED_ARRAY_FLOAT32,true,&state)||state.bytes!=16||
       !shade_buffer(ctx,argv[2],JS_TYPED_ARRAY_FLOAT32,false,&impulse)||impulse.bytes!=8)
        return invalid(ctx,op,"expected prepared Float64[14], state Float32[4], impulse Float32[2]");
    ayatsuri_follow_coeff c;ayatsuri_follow_state s;float j[2];
    memcpy(&c,coeff.data,sizeof c);memcpy(&s,state.data,sizeof s);memcpy(j,impulse.data,sizeof j);
    const ayatsuri_status status=ayatsuri_follow_impulse(&c,j,&s);
    if(status==AYATSURI_RANGE)return fail(ctx,op,POCKET_ERR_LIMIT_EXCEEDED,"float32 impulse overflow");
    if(status!=AYATSURI_OK)return invalid(ctx,op,"invalid follow impulse");
    memcpy(state.data,&s,sizeof s);return JS_UNDEFINED;
}
static JSValue js_ayatsuri_follow_drive(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;const char *op="ayatsuri.followDrive";shade_view phase,increment,output;
    if(argc!=3||!shade_buffer(ctx,argv[0],JS_TYPED_ARRAY_UINT32,true,&phase)||phase.bytes!=12||
       !shade_buffer(ctx,argv[1],JS_TYPED_ARRAY_UINT32,false,&increment)||increment.bytes!=12||
       !shade_buffer(ctx,argv[2],JS_TYPED_ARRAY_FLOAT32,true,&output)||output.bytes!=4||
       shade_overlap(&phase,&output))
        return invalid(ctx,op,"expected phase/increment Uint32[3], nonoverlapping output Float32[1]");
    uint32_t p[3],inc[3];float value;
    memcpy(p,phase.data,sizeof p);memcpy(inc,increment.data,sizeof inc);
    if(ayatsuri_follow_drive(p,inc,&value)!=AYATSURI_OK)return invalid(ctx,op,"invalid drive");
    memcpy(output.data,&value,sizeof value);memcpy(phase.data,p,sizeof p);return JS_UNDEFINED;
}
static esp_err_t ayatsuri_namespace_fill(JSContext *ctx,JSValueConst ns,void *unused){
    (void)unused;
    static const JSCFunctionListEntry methods[]={JS_CFUNC_DEF("compose",3,js_ayatsuri_compose),
        JS_CFUNC_DEF("composeNode",2,js_ayatsuri_compose_node),
        JS_CFUNC_DEF("followPrepare",2,js_ayatsuri_follow_prepare),
        JS_CFUNC_DEF("followStep",4,js_ayatsuri_follow_step),
        JS_CFUNC_DEF("followImpulse",3,js_ayatsuri_follow_impulse),
        JS_CFUNC_DEF("followDrive",3,js_ayatsuri_follow_drive),
        JS_CFUNC_DEF("followCreate",2,js_ayatsuri_follow_create),
        JS_CFUNC_DEF("driveCreate",1,js_ayatsuri_drive_create)};
    return JS_SetPropertyFunctionList(ctx,ns,methods,sizeof methods/sizeof methods[0])<0?ESP_ERR_NO_MEM:ESP_OK;
}
static bool shade_unit(float *x,float *y,float *z){
    /* Scale first: a finite nonzero Float32 direction can be subnormal. */
    const float scale=fmaxf(fabsf(*x),fmaxf(fabsf(*y),fabsf(*z)));
    if(!(scale>0)||!isfinite(scale))return false;
    *x/=scale;*y/=scale;*z/=scale;
    const float length=sqrtf((*x)*(*x)+(*y)*(*y)+(*z)*(*z));
    *x/=length;*y/=length;*z/=length;
    return true;
}
static unsigned shade_channel(float base,float light,float tint,float front,float spec,
                              float back,float thin,const float *m,unsigned max){
    float v=base*(m[0]+m[1]*front*light)+light*(m[2]*spec+m[4]*thin*back*tint);
    if(v<0)v=0;
    if(v>1)v=1;
    return (unsigned)lroundf(v*(float)max);
}
/* shade(normals, albedo, thickness, lighting, material, output[,from[,count]])
 * A bounded vertex/cell kernel, independent of frame drawing. Normals are
 * SNORM16 rather than quantised positions; float remains for transforms. */
static JSValue js_shape_shade(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;const char *op="shapes.shade";
    if(argc<6||argc>8)return invalid(ctx,op,"expected normals, albedo, thickness, lighting, material, output[, from[, count]]");
    double from=0,count=-1;
    if((argc>6&&(!number(ctx,argv[6],&from)||!whole(from,0,SHAPE_POINTS)))||
       (argc>7&&(!number(ctx,argv[7],&count)||!whole(count,0,SHAPE_POINTS))))
        return invalid(ctx,op,"from and count must be integers 0..1024");
    shade_view v[6];
    const JSTypedArrayEnum types[6]={JS_TYPED_ARRAY_INT16,JS_TYPED_ARRAY_UINT16,
        JS_TYPED_ARRAY_UINT8,JS_TYPED_ARRAY_FLOAT32,JS_TYPED_ARRAY_FLOAT32,JS_TYPED_ARRAY_UINT16};
    for(unsigned i=0;i<6;i++)if(!shade_buffer(ctx,argv[i],types[i],i==5,&v[i]))
        return invalid(ctx,op,"expected Int16, Uint16, Uint8, Float32, Float32, writable Uint16 arrays");
    const size_t n=v[1].bytes/2;
    if(n>SHAPE_POINTS)return fail(ctx,op,POCKET_ERR_LIMIT_EXCEEDED,"at most 1024 shading samples");
    if(v[0].bytes!=n*6||v[2].bytes!=n||v[3].bytes!=9*4||v[4].bytes!=8*4||v[5].bytes!=n*2)
        return invalid(ctx,op,"buffer lengths must be normals 3n, albedo n, thickness n, lighting 9, material 8, output n");
    const size_t begin=(size_t)from,len=count<0?(begin<n?n-begin:0):(size_t)count;
    if(begin>n||len>n-begin)return fail(ctx,op,POCKET_ERR_LIMIT_EXCEEDED,"sample range past buffers");
    for(unsigned i=0;i<5;i++)if(shade_overlap(&v[5],&v[i])&&
        !(i==1&&v[5].data==v[1].data&&v[5].bytes==v[1].bytes))
        return invalid(ctx,op,"output may only overlap an exactly identical albedo view");
    const float *light=(const float *)v[3].data,*m=(const float *)v[4].data;
    for(unsigned i=0;i<9;i++)if(!isfinite(light[i])||
        (i<6?(light[i]<-1||light[i]>1):(light[i]<0||light[i]>1)))
        return invalid(ctx,op,"lighting directions must be -1..1, RGB 0..1, all finite");
    for(unsigned i=0;i<8;i++)if(!isfinite(m[i])||
        (i==3?(m[i]<1||m[i]>128):(m[i]<0||m[i]>1)))
        return invalid(ctx,op,"material coefficients must be 0..1, shininess 1..128");
    float lx=light[0],ly=light[1],lz=light[2],vx=light[3],vy=light[4],vz=light[5];
    if(!shade_unit(&lx,&ly,&lz)||!shade_unit(&vx,&vy,&vz))
        return invalid(ctx,op,"light and view directions must be nonzero");
    float hx=lx+vx,hy=ly+vy,hz=lz+vz;
    const bool half=shade_unit(&hx,&hy,&hz);
    const int16_t *normal=(const int16_t *)v[0].data;
    /* Validate the requested range before its first write: even a late bad
     * normal leaves all output unchanged. Unrequested samples are untouched. */
    for(size_t i=begin;i<begin+len;i++){
        const int16_t x=normal[3*i],y=normal[3*i+1],z=normal[3*i+2];
        if(x==INT16_MIN||y==INT16_MIN||z==INT16_MIN||(!x&&!y&&!z))
            return invalid(ctx,op,"normals must be nonzero SNORM16 (-32767..32767)");
    }
    const uint16_t *albedo=(const uint16_t *)v[1].data;
    const uint8_t *thickness=v[2].data;
    uint16_t *output=(uint16_t *)v[5].data;
    for(size_t i=begin;i<begin+len;i++){
        float x=normal[3*i]*(1.0f/32767),y=normal[3*i+1]*(1.0f/32767),z=normal[3*i+2]*(1.0f/32767);
        (void)shade_unit(&x,&y,&z);
        const float dot=x*lx+y*ly+z*lz,front=fmaxf(0,dot),back=fmaxf(0,-dot);
        const float nh=half?fmaxf(0,x*hx+y*hy+z*hz):0;
        const float spec=front>0&&nh>0?powf(nh,m[3]):0,thin=1-thickness[i]*(1.0f/255);
        const uint16_t c=albedo[i];
        const unsigned r=shade_channel(((c>>11)&31)*(1.0f/31),light[6],m[5],front,spec,back,thin,m,31);
        const unsigned g=shade_channel(((c>>5)&63)*(1.0f/63),light[7],m[6],front,spec,back,thin,m,63);
        const unsigned b=shade_channel((c&31)*(1.0f/31),light[8],m[7],front,spec,back,thin,m,31);
        output[i]=(uint16_t)((r<<11)|(g<<5)|b);
    }
    return JS_UNDEFINED;
}

/* Freeze one small numeric patch into the candidate, never the caller's
 * mutable buffers. The rasterizer evaluates its continuous surface by band. */
static JSValue js_shape_ellipsoid(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;const char *op="shapes.ellipsoid";
    if(argc!=6)return invalid(ctx,op,"expected screen8, normal9, light9, material8, surface5, texture9");
    shade_view v[6];
    const size_t sizes[6]={32,36,36,32,10,36};
    for(unsigned i=0;i<6;i++)if(!shade_buffer(ctx,argv[i],i==4?JS_TYPED_ARRAY_UINT16:JS_TYPED_ARRAY_FLOAT32,false,&v[i])||v[i].bytes!=sizes[i])
        return invalid(ctx,op,"expected Float32 arrays 8/9/9/8, Uint16 surface5, Float32 texture9");
    ksn_proc_ellipsoid p={0};
    memcpy(p.screen,v[0].data,32);memcpy(p.normal,v[1].data,36);
    memcpy(p.light,v[2].data,36);memcpy(p.material,v[3].data,32);
    memcpy(p.surface,v[4].data,10);memcpy(p.texture,v[5].data,36);
    pocket_proc_mark mark;
    JSValue r=pocket_proc_native_begin(ctx,op,&mark);
    if(JS_IsException(r))return r;
    r=pocket_proc_native_ellipsoid(ctx,op,&p);
    if(JS_IsException(r))pocket_proc_native_rollback(&mark);
    return r;
}

/* The packed input avoids retaining separate views for each analytic field.
 * Byte offsets are wire layout, not C layout; even an unaligned source is
 * copied into the same aligned, zero-padded snapshot as ellipsoid(). */
static JSValue js_shape_ellipsoid_packed(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;const char *op="shapes.ellipsoidPacked";
    double offset;shade_view light;
    if(argc!=3||!JS_IsArrayBuffer(argv[0])||!number(ctx,argv[1],&offset)||!whole(offset,0,UINT32_MAX)||
       !shade_buffer(ctx,argv[2],JS_TYPED_ARRAY_FLOAT32,false,&light)||light.bytes!=36)
        return invalid(ctx,op,"expected ArrayBuffer, integer byte offset, Float32 light9");
    size_t size=0;const uint8_t *data=JS_GetArrayBuffer(ctx,&size,argv[0]);
    const size_t start=(size_t)offset;
    if(!data||start>size||148u>size-start)return invalid(ctx,op,"packed ellipsoid needs 148 bytes at offset");
    data+=start;
    ksn_proc_ellipsoid p={0};
    memcpy(p.screen,data,32);memcpy(p.normal,data+32,36);memcpy(p.material,data+68,32);
    memcpy(p.texture,data+100,36);memcpy(p.surface,data+136,10);memcpy(p.light,light.data,36);
    pocket_proc_mark mark;
    JSValue result=pocket_proc_native_begin(ctx,op,&mark);if(JS_IsException(result))return result;
    result=pocket_proc_native_ellipsoid(ctx,op,&p);
    if(JS_IsException(result))pocket_proc_native_rollback(&mark);
    return result;
}

static JSValue js_shape_blend_ellipsoids(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;const char *op="shapes.blendEllipsoids";
    if(argc!=5)return invalid(ctx,op,"expected lobes21n, light9, material11, surface3, controls2");
    shade_view v[5];
    const size_t sizes[5]={0,36,44,6,8};
    for(unsigned i=0;i<5;i++)if(!shade_buffer(ctx,argv[i],i==3?JS_TYPED_ARRAY_UINT16:JS_TYPED_ARRAY_FLOAT32,false,&v[i])||
        (i&&v[i].bytes!=sizes[i]))return invalid(ctx,op,"expected Float32 lobes/light/material, Uint16 surface3, Float32 controls2");
    if(!v[0].bytes||v[0].bytes%sizeof(ksn_proc_blend_lobe))return invalid(ctx,op,"lobes must contain complete 21-float rows");
    const size_t count=v[0].bytes/sizeof(ksn_proc_blend_lobe);
    if(count>KSN_PROC_BLEND_LOBES)return fail(ctx,op,POCKET_ERR_LIMIT_EXCEEDED,"at most eight blended ellipsoids");
    ksn_proc_blend_ellipsoids p={0};p.header.count=(uint8_t)count;
    memcpy(p.lobes,v[0].data,v[0].bytes);memcpy(p.header.light,v[1].data,36);
    memcpy(p.header.material,v[2].data,44);memcpy(p.header.surface,v[3].data,6);memcpy(p.header.controls,v[4].data,8);
    pocket_proc_mark mark;
    JSValue result=pocket_proc_native_begin(ctx,op,&mark);if(JS_IsException(result))return result;
    result=pocket_proc_native_blend_ellipsoids(ctx,op,&p);
    if(JS_IsException(result))pocket_proc_native_rollback(&mark);
    return result;
}

/* ---- fill */
/* A convex polygon's span on row y; false if none. */
static bool span(const poly *q,float y,float *l,float *r){
    float lo=1e9f,hi=-1e9f;
    if(q->empty)return false;
    for(unsigned k=0;k<q->count;k++){
        const float a0=X(q,k),a1=Y(q,k),b0=X(q,k+1),b1=Y(q,k+1);
        if((a1-y)*(b1-y)<=0&&a1!=b1){
            const float x=a0+(b0-a0)*(y-a1)/(b1-a1);
            if(x<lo)lo=x;
            if(x>hi)hi=x;
        }else if(a1==y&&b1==y){
            /* A horizontal edge on the row: both its ends. */
            if(a0<lo)lo=a0;
            if(a0>hi)hi=a0;
            if(b0<lo)lo=b0;
            if(b0>hi)hi=b0;
        }
    }
    *l=lo;*r=hi;
    return lo<=hi;
}
typedef struct {
    poly base;
    bool ellipse;
    float cx,cy,rx,ry;
    const poly *holes;
    unsigned nh;
    uint16_t color;
    pocket_proc_mark mark;
} fill_job;
/* Row y: the base's span less the holes', clipped to the screen, each piece a
 * segment. */
static JSValue fill_row(JSContext *ctx,const char *op,const fill_job *j,float y){
    float l,r;
    if(j->ellipse){
        const float v=(y-j->cy)/j->ry,q=1-v*v;
        if(!(q>=0))return JS_UNDEFINED;
        const float w=j->rx*sqrtf(q);
        l=j->cx-w;r=j->cx+w;
    }else if(!span(&j->base,y,&l,&r))return JS_UNDEFINED;
    float hs[SHAPE_HOLES][2];
    unsigned n=0;
    for(unsigned k=0;k<j->nh;k++){
        float a,b;
        if(!span(&j->holes[k],y,&a,&b))continue;
        unsigned i=n++;
        while(i&&hs[i-1][0]>a){hs[i][0]=hs[i-1][0];hs[i][1]=hs[i-1][1];--i;}
        hs[i][0]=a;hs[i][1]=b;
    }
    float x=l<0?0:l;
    const float e=r>SHAPE_W-1?SHAPE_W-1:r;
    const int row=(int)y;
    for(unsigned k=0;k<=n;k++){
        const float stop=k<n?(hs[k][0]<e?hs[k][0]:e):e;
        if(stop>x){
            JSValue res=pocket_proc_native_line(ctx,op,(int)lroundf(x),row,(int)lroundf(stop),row,j->color);
            if(JS_IsException(res))return res;
        }
        if(k<n&&hs[k][1]>x)x=hs[k][1];
        if(x>e)break;
    }
    return JS_UNDEFINED;
}
/* fill(points, shape, holes, color[, {top, bottom, step}]) */
static JSValue js_shape_fill(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *op="shapes.fill";
    if(argc<4||argc>5)return invalid(ctx,op,"expected points, shape, holes, color[, options]");
    double color,top=0,bottom=0,step=1,e[4];
    bool has_top,has_bottom;
    uint32_t hp[2*SHAPE_HOLES];
    unsigned nh,ns=0;
    if(!number(ctx,argv[3],&color)||!whole(color,0,65535))return invalid(ctx,op,"color must be RGB565 0..65535");
    if(!pairs(ctx,argv[2],hp,SHAPE_HOLES,&nh))
        return invalid(ctx,op,"holes must be [start, count, ...]: up to 6 polygons of 3..256 points");
    const JSValueConst options=argc>4?argv[4]:JS_UNDEFINED;
    if(argc>4&&!JS_IsUndefined(options)&&!JS_IsObject(options))return invalid(ctx,op,"options must be an object");
    if(!option(ctx,options,"top",&top,&has_top)||!option(ctx,options,"bottom",&bottom,&has_bottom)||
       !option(ctx,options,"step",&step,NULL)||(has_top&&!isfinite(top))||(has_bottom&&!isfinite(bottom))||
       (step!=1&&step!=2))
        return invalid(ctx,op,"options: top and bottom finite rows, step 1 or 2");
    if(JS_IsArray(argv[1])){
        JSValue l=JS_GetPropertyStr(ctx,argv[1],"length");
        double len;
        bool ok=!JS_IsException(l)&&number(ctx,l,&len)&&(len==2||len==4);
        JS_FreeValue(ctx,l);
        ns=ok?(unsigned)len:0;
        for(unsigned i=0;i<ns;i++){
            JSValue v=JS_GetPropertyUint32(ctx,argv[1],i);
            ok=!JS_IsException(v)&&number(ctx,v,&e[i])&&isfinite(e[i]);
            JS_FreeValue(ctx,v);
            if(!ok)ns=0;
        }
    }
    if(ns==2&&!(whole(e[0],0,SHAPE_POINTS-1)&&whole(e[1],3,SHAPE_POLY)))ns=0;
    if(ns==4&&!(e[2]>0&&e[3]>0))ns=0;
    if(!ns)return invalid(ctx,op,"shape must be [start, count] (3..256 points) or [cx, cy, rx, ry] (radii > 0)");
    buffer pts;
    if(!view(ctx,argv[0],2,&pts))return invalid(ctx,op,"points must be a mutable Float32Array");
    poly holes[SHAPE_HOLES];
    fill_job j={.ellipse=ns==4,.holes=holes,.nh=nh,.color=(uint16_t)color};
    const uint32_t bp[2]={(uint32_t)e[0],(uint32_t)e[1]};
    if(!polys(hp,nh,&pts,holes)||(ns==2&&!polys(bp,1,&pts,&j.base)))
        return fail(ctx,op,POCKET_ERR_LIMIT_EXCEEDED,"a polygon runs past the end of points");
    float lo,hi;
    if(j.ellipse){
        j.cx=(float)e[0];j.cy=(float)e[1];j.rx=(float)e[2];j.ry=(float)e[3];
        lo=j.cy-j.ry;hi=j.cy+j.ry;
    }else{
        if(j.base.empty)return JS_UNDEFINED;
        lo=1e9f;hi=-1e9f;
        for(unsigned k=0;k<j.base.count;k++){const float y=Y(&j.base,k);if(y<lo)lo=y;if(y>hi)hi=y;}
    }
    if(has_top&&(float)top>lo)lo=(float)top;
    if(has_bottom&&(float)bottom<hi)hi=(float)bottom;
    float y0=ceilf(lo),y1=floorf(hi);
    if(y0<0)y0=0;
    if(y1>SHAPE_H-1)y1=SHAPE_H-1;
    if(step==2&&((int)y0&1))y0+=1;
    JSValue r=pocket_proc_native_begin(ctx,op,&j.mark);
    if(JS_IsException(r))return r;
    for(float y=y0;y<=y1;y+=(float)step){
        r=fill_row(ctx,op,&j,y);
        if(JS_IsException(r)){pocket_proc_native_rollback(&j.mark);return r;}
    }
    return JS_UNDEFINED;
}

/* ---- outline */
static bool inside(const poly *q,float px,float py){
    if(q->empty)return false;
    unsigned pos=0,neg=0;
    for(unsigned k=0;k<q->count;k++){
        const float x=(X(q,k+1)-X(q,k))*(py-Y(q,k))-(Y(q,k+1)-Y(q,k))*(px-X(q,k));
        if(x>0)++pos;else if(x<0)++neg;
    }
    return !pos||!neg;
}
/* Liang-Barsky to the screen's pixel centres: the part of a..b on screen;
 * false if none. */
static bool clip(float *ax,float *ay,float *bx,float *by){
    float t0=0,t1=1;
    const float dx=*bx-*ax,dy=*by-*ay;
    const float p[4]={-dx,dx,-dy,dy},q[4]={*ax,SHAPE_W-1-*ax,*ay,SHAPE_H-1-*ay};
    for(unsigned i=0;i<4;i++){
        if(p[i]==0){if(q[i]<0)return false;continue;}
        const float t=q[i]/p[i];
        if(p[i]<0){if(t>t1)return false;if(t>t0)t0=t;}
        else{if(t<t0)return false;if(t<t1)t1=t;}
    }
    const float x0=*ax,y0=*ay;
    *ax=x0+t0*dx;*ay=y0+t0*dy;*bx=x0+t1*dx;*by=y0+t1*dy;
    return true;
}
/* outline(points, start, count, color[, {closed, pieces, hidden}]) */
static JSValue js_shape_outline(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *op="shapes.outline";
    if(argc<4||argc>5)return invalid(ctx,op,"expected points, start, count, color[, options]");
    double start,count,color,closed=0,pieces=1;
    uint32_t hp[2*SHAPE_HOLES];
    unsigned nh=0;
    if(!number(ctx,argv[1],&start)||!whole(start,0,SHAPE_POINTS-1)||
       !number(ctx,argv[2],&count)||!whole(count,2,SHAPE_POLY))
        return invalid(ctx,op,"start 0..1023 and count 2..256 required");
    if(!number(ctx,argv[3],&color)||!whole(color,0,65535))return invalid(ctx,op,"color must be RGB565 0..65535");
    const JSValueConst options=argc>4?argv[4]:JS_UNDEFINED;
    if(argc>4&&!JS_IsUndefined(options)&&!JS_IsObject(options))return invalid(ctx,op,"options must be an object");
    JSValue hidden=JS_IsUndefined(options)?JS_UNDEFINED:JS_GetPropertyStr(ctx,options,"hidden");
    if(JS_IsException(hidden))return hidden;
    const bool ok=pairs(ctx,hidden,hp,SHAPE_HOLES,&nh);
    JS_FreeValue(ctx,hidden);
    if(!ok||!option(ctx,options,"closed",&closed,NULL)||!option(ctx,options,"pieces",&pieces,NULL)||
       !whole(pieces,1,8))
        return invalid(ctx,op,"options: closed, pieces 1..8, hidden [start, count, ...] (up to 6)");
    buffer pts;
    if(!view(ctx,argv[0],2,&pts))return invalid(ctx,op,"points must be a mutable Float32Array");
    poly hide[SHAPE_HOLES];
    const poly line={&pts,(unsigned)start,(unsigned)count,false};
    if(line.start+line.count>pts.points||!polys(hp,nh,&pts,hide))
        return fail(ctx,op,POCKET_ERR_LIMIT_EXCEEDED,"a polygon runs past the end of points");
    pocket_proc_mark mark;
    JSValue r=pocket_proc_native_begin(ctx,op,&mark);
    if(JS_IsException(r))return r;
    const unsigned k=(unsigned)pieces,edges=closed!=0?line.count:line.count-1u;
    for(unsigned e=0;e<edges;e++){
        const float a0=X(&line,e),a1=Y(&line,e),b0=X(&line,e+1),b1=Y(&line,e+1);
        if(!isfinite(a0)||!isfinite(a1)||!isfinite(b0)||!isfinite(b1))continue;
        for(unsigned j=0;j<k;j++){
            float p0=a0+(b0-a0)*(float)j/(float)k,p1=a1+(b1-a1)*(float)j/(float)k,
                  q0=a0+(b0-a0)*(float)(j+1)/(float)k,q1=a1+(b1-a1)*(float)(j+1)/(float)k;
            bool hid=false;
            for(unsigned h=0;h<nh&&!hid;h++)hid=inside(&hide[h],(p0+q0)/2,(p1+q1)/2);
            if(hid||!clip(&p0,&p1,&q0,&q1))continue;
            r=pocket_proc_native_line(ctx,op,(int)lroundf(p0),(int)lroundf(p1),
                                      (int)lroundf(q0),(int)lroundf(q1),(uint16_t)color);
            if(JS_IsException(r)){pocket_proc_native_rollback(&mark);return r;}
        }
    }
    return JS_UNDEFINED;
}

esp_err_t pocket_shape_fill(JSContext *ctx,JSValueConst ns,void *unused){
    (void)unused;
    static const JSCFunctionListEntry methods[]={
        JS_CFUNC_DEF("project",3,js_shape_project),
        JS_CFUNC_DEF("shade",6,js_shape_shade),
        JS_CFUNC_DEF("ellipsoid",6,js_shape_ellipsoid),
        JS_CFUNC_DEF("ellipsoidPacked",3,js_shape_ellipsoid_packed),
        JS_CFUNC_DEF("blendEllipsoids",5,js_shape_blend_ellipsoids),
        JS_CFUNC_DEF("fill",4,js_shape_fill),
        JS_CFUNC_DEF("outline",4,js_shape_outline),
    };
    return JS_SetPropertyFunctionList(ctx,ns,methods,(int)(sizeof methods/sizeof methods[0]))<0?
           ESP_ERR_NO_MEM:ESP_OK;
}
/* Every number here is refused past by the functions. */
static const pocket_limit_t shape_limits[]={
    {.name="maxPoints",.kind=POCKET_LIMIT_INT,.number=SHAPE_POINTS},
    {.name="maxPolygonPoints",.kind=POCKET_LIMIT_INT,.number=SHAPE_POLY},
    {.name="maxHoles",.kind=POCKET_LIMIT_INT,.number=SHAPE_HOLES},
    {.name="maxShadeSamples",.kind=POCKET_LIMIT_INT,.number=SHAPE_POINTS},
    {.name="ellipsoidEntries",.kind=POCKET_LIMIT_INT,.number=KSN_PROC_ELLIPSOID_ENTRIES},
    {.name="blendEllipsoidLobes",.kind=POCKET_LIMIT_INT,.number=KSN_PROC_BLEND_LOBES},
    {0},
};
static const pocket_capability_t capability={.name="kasane.shapes",.supported=true,
    .available=true,.limits=shape_limits};
/* Both names resolve to one namespace: no second method table or work owner. */
static JSValue js_hone_namespace(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)argc;(void)argv;
    JSValue ns=JS_GetPropertyStr(ctx,self,"shapes");
    if(JS_IsException(ns))return ns;
    if(!JS_IsObject(ns)){
        JS_FreeValue(ctx,ns);
        return invalid(ctx,"hone","shapes namespace unavailable");
    }
    if(JS_DefinePropertyValueStr(ctx,self,"hone",JS_DupValue(ctx,ns),JS_PROP_ENUMERABLE)<0){
        JS_FreeValue(ctx,ns);return JS_EXCEPTION;
    }
    return ns;
}
esp_err_t pocket_shape_install(JSContext *ctx,void *unused){
    (void)unused;
    esp_err_t err=pocket_api_register(&capability);
    if(err!=ESP_OK)return err;
    err=pocket_api_lazy(ctx,"shapes",pocket_shape_fill,NULL);
    if(err!=ESP_OK)return err;
    JSValue root=pocket_api_root(ctx);
    if(!JS_IsObject(root)){JS_FreeValue(ctx,root);return ESP_ERR_INVALID_STATE;}
    JSValue getter=JS_NewCFunction(ctx,js_hone_namespace,"hone",0);
    if(JS_IsException(getter)){JS_FreeValue(ctx,root);return ESP_ERR_NO_MEM;}
    JSAtom atom=JS_NewAtom(ctx,"hone");
    if(atom==JS_ATOM_NULL){JS_FreeValue(ctx,getter);JS_FreeValue(ctx,root);return ESP_ERR_NO_MEM;}
    int ok=JS_DefinePropertyGetSet(ctx,root,atom,getter,JS_UNDEFINED,
                                JS_PROP_ENUMERABLE|JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx,atom);JS_FreeValue(ctx,root);
    if(ok<0)return ESP_ERR_NO_MEM;
    return pocket_api_lazy(ctx,"ayatsuri",ayatsuri_namespace_fill,NULL);
}
