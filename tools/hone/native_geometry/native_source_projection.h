#ifndef HONE_NATIVE_SOURCE_PROJECTION_H
#define HONE_NATIVE_SOURCE_PROJECTION_H
#include "ayatsuri/ayatsuri_transform.h"
#include "ayatsuri/ayatsuri_visibility.h"
#include <stdbool.h>
#include <stdint.h>
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#if defined(CONFIG_IDF_TARGET_ESP32S3) && !defined(HONE_SOURCE_PROJECTION_PORTABLE)
#include "source_projection_s3.h"
#endif

typedef struct {
    float axis[3][3], pivot[3], zoom, center[2];
} hone_source_camera;

/* Match the stream's orthographic Z-up basis, including its polar fallback
 * and the narrowing at binary32 stores. The caller owns all output scratch. */
static inline bool hone_source_camera_z_up(const float camera[9],hone_source_camera *out) {
    if(!camera||!out)return false;
    for(unsigned k=0;k<9;k++)if(!isfinite(camera[k]))return false;
    if(!(camera[6]>0))return false;
    double x=(double)camera[0]-camera[3],y=(double)camera[1]-camera[4],z=(double)camera[2]-camera[5];
    double distance=hypot(hypot(x,y),z);
    if(!(distance>0) || !isfinite(distance))return false;
    x/=distance;y/=distance;z/=distance;
    double horizontal=hypot(x,y);
    double u=horizontal<1e-6?1:-y/horizontal,v=horizontal<1e-6?0:x/horizontal;
    *out=(hone_source_camera){
        .axis={{(float)u,(float)v,0},{(float)(v*z),(float)(-u*z),(float)(u*y-v*x)},
               {(float)x,(float)y,(float)z}},
        .pivot={camera[3],camera[4],camera[5]},.zoom=camera[6],.center={camera[7],camera[8]}
    };
    return true;
}

static inline bool hone_source_projection(const hone_source_camera *camera,const ayatsuri_transform *world,
                                           float projection[12]) {
    if(!camera||!world||!projection)return false;
    const float *m=world->m;
    const double a=m[0],b=m[1],c=m[2],e=m[4],f=m[5],g=m[6],h=m[8],j=m[9],k=m[10];
    const double det=a*(f*k-g*j)-b*(e*k-g*h)+c*(e*j-f*h);
    if(!isfinite(det)||det==0)return false;
    for(unsigned r=0;r<3;r++){
        const float *v=camera->axis[r];
        float zoom=r<2?camera->zoom:1;
        for(unsigned col=0;col<3;col++)
            projection[4*r+col]=zoom*(v[0]*m[col]+v[1]*m[4+col]+v[2]*m[8+col]);
        projection[4*r+3]=zoom*(v[0]*(m[3]-camera->pivot[0])+v[1]*(m[7]-camera->pivot[1])+
                                 v[2]*(m[11]-camera->pivot[2]));
        if(r<2)projection[4*r+3]+=camera->center[r];
    }
    for(unsigned k=0;k<12;k++)if(!isfinite(projection[k]))return false;
    return true;
}

/* Every local XYZ belongs to one source vertex. Split authored draws with
 * the same world projection can borrow the completed array until it is reused.
 * All spans are nonoverlapping and sized by the caller. Discard scratch if
 * validation fails partway through a mesh. */
static inline bool hone_source_project_vertices(const float p[12],const float *local,
                                                 size_t count,float out[][3]) {
    if(!p||!local||!out||!count||count>256)return false;
#if defined(CONFIG_IDF_TARGET_ESP32S3) && !defined(HONE_SOURCE_PROJECTION_PORTABLE)
    return hone_source_project_s3(p,local,count,out);
#else
    for(size_t n=0;n<count;n++,local+=3){
        float a=local[0],b=local[1],c=local[2];
        if(!(fabsf(a)<=4096 && fabsf(b)<=4096 && fabsf(c)<=4096))return false;
        for(unsigned j=0;j<3;j++)out[n][j]=p[4*j]*a+p[4*j+1]*b+p[4*j+2]*c+p[4*j+3];
        if(!(fabsf(out[n][0])<=4096 && fabsf(out[n][1])<=4096 && isfinite(out[n][2])))return false;
    }
    return true;
#endif
}

/* The authored-order filtered test mirrors ayatsuri_visibility_select_faces:
 * ambiguous binary32 estimates use the original binary64 threshold. */
static inline unsigned hone_source_winding(const float *a,const float *b,const float *c) {
    float left=(b[0]-a[0])*(c[1]-a[1]),right=(b[1]-a[1])*(c[0]-a[0]),estimate=left-right;
    float error=1.000001e-5f+0x1p-19f*(fabsf(left)+fabsf(right));
    if(estimate>error)return AYATSURI_VISIBILITY_POSITIVE;
    if(estimate< -error)return AYATSURI_VISIBILITY_NEGATIVE;
    double area=((double)b[0]-a[0])*((double)c[1]-a[1])-((double)b[1]-a[1])*((double)c[0]-a[0]);
    return area>1e-5?AYATSURI_VISIBILITY_POSITIVE:area< -1e-5?AYATSURI_VISIBILITY_NEGATIVE:0;
}

/* Faces store three U8 source-vertex IDs and a material ID. Asset admission
 * must prove each maps to the original authored triple/program at open, and
 * verify its indices against the source vertex count. The callback receives
 * the original face index, preserves local coordinates for clipping/shading,
 * and synchronously consumes projected corners. No sort or winding rewrite. */
typedef bool (*hone_source_face_sink)(void *context,unsigned original,const uint8_t face[4],
                                      const float *local,const float xyz[9],bool inside);
static inline bool hone_source_faces_valid(const uint8_t faces[][4],unsigned count,
                                          size_t vertices,unsigned materials) {
    if(!faces||!count||!vertices||vertices>256||!materials||materials>256)return false;
    for(unsigned i=0;i<count;i++){
        const uint8_t *f=faces[i];
        if(f[0]>=vertices||f[1]>=vertices||f[2]>=vertices||f[3]>=materials)return false;
    }
    return true;
}
static inline bool hone_source_emit_faces(const uint8_t faces[][4],unsigned count,
        ayatsuri_visibility_faces mode,const float projected[][3],const float *local,
        float width,float height,hone_source_face_sink sink,void *context) {
    if(!faces||!projected||!local||!sink||mode<AYATSURI_VISIBILITY_POSITIVE||
       mode>AYATSURI_VISIBILITY_BOTH||!(width>0)||!(height>0))return false;
    for(unsigned i=0;i<count;i++){
        const uint8_t *f=faces[i];
        const float *a=projected[f[0]],*b=projected[f[1]],*c=projected[f[2]];
        if(!(mode&hone_source_winding(a,b,c)))continue;
        bool inside=true;
        float xyz[9];
        for(unsigned k=0;k<3;k++){
            const float *v=projected[f[k]];
            if(!(fabsf(v[2])<=1e20f))return false;
            if(v[0]<0||v[0]>width||v[1]<0||v[1]>height)inside=false;
            memcpy(xyz+3*k,v,3*sizeof *v);
        }
        if(!sink(context,i,f,local,xyz,inside))return false;
    }
    return true;
}
#endif
