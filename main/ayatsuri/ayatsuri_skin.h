#ifndef AYATSURI_SKIN_H
#define AYATSURI_SKIN_H

#include "ayatsuri_transform.h"
#include <stdint.h>

/* Experimental scalar core, NOT an AYNM extension or a render/JS API.
 * Same arithmetic/coordinate contract as ayatsuri_transform.h. The caller
 * supplies joint globals (including the hierarchy) and inverse bind matrices
 * in one common mesh space. Palette[j] = global[j] * inverse_bind[j].
 * Four U16 weights must sum exactly to 65535, including zero-weight slots;
 * every joint index must be valid. No implicit normalization or fallback.
 *
 * All spans are caller-owned, correctly aligned, live and immutable during
 * the call. Output must be disjoint from every input; input spans may alias.
 * Two passes validate the complete result before publishing any output byte.
 * No allocation, retained pointers, callbacks, clock, or variable-size stack.
 * Zero counts are invalid. At most 65536 joints are addressable by U16.
 *
 * Normals use the inverse transpose of each vertex's blended linear matrix,
 * then normalize. This is the fixed-weight local-affine approximation, not
 * exact normals of a surface with spatially varying weights. A singular blend
 * rejects the entire batch, even when each joint transform is invertible.
 * Reflections preserve outward normals; triangle winding is not rewritten. */
typedef struct {
    float position[3], normal[3];
    uint16_t joints[4], weights[4];
} ayatsuri_skin_vertex;
typedef struct { float position[3], normal[3]; } ayatsuri_skin_result;

static inline int ayatsuri_skin_span(const void *p,size_t count,size_t stride,
                                    uintptr_t *begin,uintptr_t *end){
    if(!p||!count||count>SIZE_MAX/stride)return 0;
    const size_t bytes=count*stride;
    *begin=(uintptr_t)p;
    if(bytes>UINTPTR_MAX-*begin)return 0;
    *end=*begin+bytes;return 1;
}
static inline int ayatsuri_skin_disjoint(const void *a,size_t na,size_t sa,
                                        const void *b,size_t nb,size_t sb){
    uintptr_t ab,ae,bb,be;
    return ayatsuri_skin_span(a,na,sa,&ab,&ae)&&
           ayatsuri_skin_span(b,nb,sb,&bb,&be)&&(ae<=bb||be<=ab);
}
static inline int ayatsuri_skin_narrow(double v,float *out){
    const double magnitude=fabs(v),midpoint=(double)FLT_MAX+0x1p103;
    if(!isfinite(v)||magnitude>=midpoint)return 0;
    *out=magnitude>(double)FLT_MAX?(v<0?-FLT_MAX:FLT_MAX):(float)v;
    return 1;
}
static inline ayatsuri_status ayatsuri_skin_palette(
        const ayatsuri_transform *global,const ayatsuri_transform *inverse_bind,
        size_t joints,ayatsuri_transform *out){
    if(joints>65536||
       !ayatsuri_skin_disjoint(out,joints,sizeof *out,global,joints,sizeof *global)||
       !ayatsuri_skin_disjoint(out,joints,sizeof *out,inverse_bind,joints,sizeof *inverse_bind))
        return AYATSURI_INVALID;
    for(unsigned pass=0;pass<2;++pass)for(size_t j=0;j<joints;++j){
        const float *a=global[j].m,*b=inverse_bind[j].m;
        ayatsuri_transform candidate;
        for(unsigned k=0;k<12;++k)if(!isfinite(a[k])||!isfinite(b[k]))return AYATSURI_INVALID;
        for(unsigned r=0;r<3;++r)for(unsigned c=0;c<4;++c){
            const double v=(double)a[4*r]*b[c]+(double)a[4*r+1]*b[4+c]+
                           (double)a[4*r+2]*b[8+c]+(c==3?a[4*r+3]:0);
            if(!ayatsuri_skin_narrow(v,&candidate.m[4*r+c]))return AYATSURI_RANGE;
        }
        if(pass)memcpy(out+j,&candidate,sizeof candidate);
    }
    return AYATSURI_OK;
}

static inline ayatsuri_status ayatsuri_skin_evaluate(
        const ayatsuri_transform *palette,size_t joints,
        const ayatsuri_skin_vertex *vertices,size_t count,ayatsuri_skin_result *out){
    if(joints>65536||
       !ayatsuri_skin_disjoint(out,count,sizeof *out,palette,joints,sizeof *palette)||
       !ayatsuri_skin_disjoint(out,count,sizeof *out,vertices,count,sizeof *vertices))
        return AYATSURI_INVALID;
    /* Validate unused palette rows too, so invalid cold input cannot hide. */
    for(size_t j=0;j<joints;++j)for(unsigned k=0;k<12;++k)
        if(!isfinite(palette[j].m[k]))return AYATSURI_INVALID;
    for(unsigned pass=0;pass<2;++pass)for(size_t v=0;v<count;++v){
        const ayatsuri_skin_vertex *in=vertices+v;
        uint32_t sum=0;
        for(unsigned k=0;k<4;++k){
            if(in->joints[k]>=joints)return AYATSURI_INVALID;
            sum+=in->weights[k];
        }
        if(sum!=65535)return AYATSURI_INVALID;
        for(unsigned k=0;k<3;++k)
            if(!isfinite(in->position[k])||!isfinite(in->normal[k]))return AYATSURI_INVALID;
        if(in->normal[0]==0&&in->normal[1]==0&&in->normal[2]==0)return AYATSURI_INVALID;
        double m[12]={0};
        for(unsigned k=0;k<4;++k)for(unsigned c=0;c<12;++c)
            m[c]+=(double)palette[in->joints[k]].m[c]*in->weights[k]/65535.0;
        const double a=m[0],b=m[1],c=m[2],d=m[4],e=m[5],f=m[6],g=m[8],h=m[9],i=m[10],
            det=a*(e*i-f*h)-b*(d*i-f*g)+c*(d*h-e*g);
        if(!isfinite(det)||det==0)return AYATSURI_RANGE;
        const double nx=in->normal[0],ny=in->normal[1],nz=in->normal[2],
            x=((e*i-f*h)*nx+(f*g-d*i)*ny+(d*h-e*g)*nz)/det,
            y=((c*h-b*i)*nx+(a*i-c*g)*ny+(b*g-a*h)*nz)/det,
            z=((b*f-c*e)*nx+(c*d-a*f)*ny+(a*e-b*d)*nz)/det,
            length=hypot(hypot(x,y),z);
        if(!isfinite(length)||!(length>0))return AYATSURI_RANGE;
        ayatsuri_skin_result candidate;
        const double normal[3]={x/length,y/length,z/length};
        for(unsigned r=0;r<3;++r){
            const double p=m[4*r]*in->position[0]+m[4*r+1]*in->position[1]+
                           m[4*r+2]*in->position[2]+m[4*r+3];
            if(!ayatsuri_skin_narrow(p,&candidate.position[r])||
               !ayatsuri_skin_narrow(normal[r],&candidate.normal[r]))return AYATSURI_RANGE;
        }
        if(pass)memcpy(out+v,&candidate,sizeof candidate);
    }
    return AYATSURI_OK;
}
#endif
