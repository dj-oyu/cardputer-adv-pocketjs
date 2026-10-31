#ifndef AYATSURI_SHADE_H
#define AYATSURI_SHADE_H
#include <math.h>
#include <stdint.h>
#include <stdbool.h>
/* Shared vertex shading arithmetic. Callers validate spans, finite ranges and
 * nonzero SNORM16 normals before publishing any output. No retained state. */
static inline bool ayatsuri_shade_unit(float *x,float *y,float *z){
    /* Scale first: a finite nonzero Float32 direction can be subnormal. */
    const float scale=fmaxf(fabsf(*x),fmaxf(fabsf(*y),fabsf(*z)));
    if(!(scale>0)||!isfinite(scale))return false;
    *x/=scale;*y/=scale;*z/=scale;
    const float length=sqrtf((*x)*(*x)+(*y)*(*y)+(*z)*(*z));
    *x/=length;*y/=length;*z/=length;
    return true;
}
static inline unsigned ayatsuri_shade_channel(float base,float light,float tint,float front,float spec,
                              float back,float thin,const float *m,unsigned max){
    float v=base*(m[0]+m[1]*front*light)+light*(m[2]*spec+m[4]*thin*back*tint);
    if(v<0)v=0;
    if(v>1)v=1;
    return (unsigned)lroundf(v*(float)max);
}

typedef struct {float lx,ly,lz,hx,hy,hz;bool half;} ayatsuri_shade_basis;
static inline bool ayatsuri_shade_prepare(const float light[9],ayatsuri_shade_basis *out){
    float lx=light[0],ly=light[1],lz=light[2],vx=light[3],vy=light[4],vz=light[5];
    if(!ayatsuri_shade_unit(&lx,&ly,&lz)||!ayatsuri_shade_unit(&vx,&vy,&vz))return false;
    float hx=lx+vx,hy=ly+vy,hz=lz+vz;
    const bool half=ayatsuri_shade_unit(&hx,&hy,&hz);
    *out=(ayatsuri_shade_basis){lx,ly,lz,hx,hy,hz,half};return true;
}
static inline uint16_t ayatsuri_shade_sample(const int16_t normal[3],uint16_t albedo,uint8_t thickness,
        const float light[9],const float m[8],const ayatsuri_shade_basis *basis){
    float x=normal[0]*(1.0f/32767),y=normal[1]*(1.0f/32767),z=normal[2]*(1.0f/32767);
    (void)ayatsuri_shade_unit(&x,&y,&z);
    const float dot=x*basis->lx+y*basis->ly+z*basis->lz,front=fmaxf(0,dot),back=fmaxf(0,-dot);
    const float nh=basis->half?fmaxf(0,x*basis->hx+y*basis->hy+z*basis->hz):0;
    const float spec=front>0&&nh>0?powf(nh,m[3]):0,thin=1-thickness*(1.0f/255);
    const unsigned r=ayatsuri_shade_channel(((albedo>>11)&31)*(1.0f/31),light[6],m[5],front,spec,back,thin,m,31);
    const unsigned g=ayatsuri_shade_channel(((albedo>>5)&63)*(1.0f/63),light[7],m[6],front,spec,back,thin,m,63);
    const unsigned b=ayatsuri_shade_channel((albedo&31)*(1.0f/31),light[8],m[7],front,spec,back,thin,m,31);
    return (uint16_t)((r<<11)|(g<<5)|b);
}
#endif
