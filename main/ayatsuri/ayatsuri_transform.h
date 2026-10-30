#ifndef AYATSURI_TRANSFORM_H
#define AYATSURI_TRANSFORM_H

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

/* Shared scalar evaluation, not a scene manager or a hone drawing API.
 * Compile with IEEE arithmetic, no fast-math and -ffp-contract=off. The
 * caller keeps round-to-nearest, ties-to-even; this function never sets a
 * rounding mode. Inputs and stored output are binary32; every
 * arithmetic intermediate is double (no excess-precision evaluation).
 * Coordinates: x right, y down, z towards the viewer; radians; column
 * vectors, T * Rz * Ry * Rx * S, then parent * local. m is row-major 3x4.
 * All storage is caller-owned and synchronously borrowed. No allocation,
 * clock, callback, hidden state or retained pointer. Failure changes no
 * output byte. out == parent is supported; concurrent mutation is not. */
#if defined(__FAST_MATH__)
#error "ayatsuri transforms require IEEE arithmetic (no fast-math)"
#endif

typedef struct { float position[3], rotation[3], scale[3]; } ayatsuri_pose;
typedef struct { float m[12]; } ayatsuri_transform;
typedef enum { AYATSURI_OK, AYATSURI_INVALID, AYATSURI_RANGE } ayatsuri_status;

#if defined(__cplusplus)
static_assert(sizeof(float)==4 && FLT_RADIX==2 && FLT_MANT_DIG==24 &&
              FLT_MAX_EXP==128 && FLT_MIN_EXP==-125 && sizeof(double)==8 &&
              DBL_MANT_DIG==53 && DBL_MAX_EXP==1024 && DBL_MIN_EXP==-1021,
              "ayatsuri requires binary32 storage and binary64 intermediates");
static_assert(sizeof(ayatsuri_pose)==36 && sizeof(ayatsuri_transform)==48,"ayatsuri storage layout");
#else
_Static_assert(sizeof(float)==4 && FLT_RADIX==2 && FLT_MANT_DIG==24 &&
               FLT_MAX_EXP==128 && FLT_MIN_EXP==-125 && sizeof(double)==8 &&
               DBL_MANT_DIG==53 && DBL_MAX_EXP==1024 && DBL_MIN_EXP==-1021,
               "ayatsuri requires binary32 storage and binary64 intermediates");
_Static_assert(sizeof(ayatsuri_pose)==36 && sizeof(ayatsuri_transform)==48,"ayatsuri storage layout");
#endif

static inline ayatsuri_status ayatsuri_transform_evaluate(
        const ayatsuri_pose *pose,const ayatsuri_transform *parent,
        ayatsuri_transform *out){
    if(!pose||!out)return AYATSURI_INVALID;
    for(unsigned k=0;k<3;++k)
        if(!isfinite(pose->position[k])||!isfinite(pose->rotation[k])||
           !isfinite(pose->scale[k])||pose->scale[k]==0)return AYATSURI_INVALID;
    if(parent)for(unsigned k=0;k<12;++k)
        if(!isfinite(parent->m[k]))return AYATSURI_INVALID;

    const double x=pose->rotation[0],y=pose->rotation[1],z=pose->rotation[2],
        cx=cos(x),sx=sin(x),cy=cos(y),sy=sin(y),cz=cos(z),sz=sin(z),
        a=cz*cy*pose->scale[0],b=(cz*sy*sx-sz*cx)*pose->scale[1],
        c=(cz*sy*cx+sz*sx)*pose->scale[2],d=sz*cy*pose->scale[0],
        e=(sz*sy*sx+cz*cx)*pose->scale[1],f=(sz*sy*cx-cz*sx)*pose->scale[2],
        g=-sy*pose->scale[0],h=cy*sx*pose->scale[1],j=cy*cx*pose->scale[2];
    double world[12];
    if(!parent){
        world[0]=a;world[1]=b;world[2]=c;world[3]=pose->position[0];
        world[4]=d;world[5]=e;world[6]=f;world[7]=pose->position[1];
        world[8]=g;world[9]=h;world[10]=j;world[11]=pose->position[2];
    }else for(unsigned k=0;k<12;k+=4){
        const float *p=parent->m;
        world[k]=p[k]*a+p[k+1]*d+p[k+2]*g;
        world[k+1]=p[k]*b+p[k+1]*e+p[k+2]*h;
        world[k+2]=p[k]*c+p[k+1]*f+p[k+2]*j;
        world[k+3]=p[k]*(double)pose->position[0]+p[k+1]*(double)pose->position[1]+
                   p[k+2]*(double)pose->position[2]+p[k+3];
    }
    ayatsuri_transform candidate;
    /* The interval just above FLT_MAX still rounds to FLT_MAX in binary32.
     * Clamp that interval explicitly: casting a double outside float's range
     * is not portable C. The midpoint itself rounds to infinity (ties-even). */
    const double midpoint=(double)FLT_MAX+0x1p103;
    for(unsigned k=0;k<12;++k){
        const double v=world[k],magnitude=fabs(v);
        if(!isfinite(v)||magnitude>=midpoint)return AYATSURI_RANGE;
        candidate.m[k]=magnitude>(double)FLT_MAX?(v<0?-FLT_MAX:FLT_MAX):(float)v;
    }
    memcpy(out,&candidate,sizeof candidate);
    return AYATSURI_OK;
}
#endif
