#include "ayatsuri_visibility.h"
#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>

#if defined(__FAST_MATH__)
#error "ayatsuri visibility requires IEEE arithmetic (no fast-math)"
#endif
_Static_assert(sizeof(float)==4&&FLT_RADIX==2&&FLT_MANT_DIG==24&&
               sizeof(double)==8&&DBL_MANT_DIG==53,"visibility requires binary32/binary64");
typedef struct {uintptr_t address;size_t bytes;} visibility_span;
static bool visibility_range(const void *p,size_t bytes,visibility_span *span){
    const uintptr_t address=(uintptr_t)p;
    if(bytes>UINTPTR_MAX-address)return false;
    *span=(visibility_span){address,bytes};return true;
}
static bool visibility_overlap(visibility_span a,visibility_span b){
    return a.bytes&&b.bytes&&(a.address<=b.address?b.address-a.address<a.bytes:a.address-b.address<b.bytes);
}
ayatsuri_visibility_status ayatsuri_visibility_select(
    const float *xy,const float *depth,size_t vertices,
    const uint16_t *indices,size_t triangles,ayatsuri_visibility_order order,
    uint16_t *out,size_t capacity,uint32_t counts[3]){
    if(!xy||!depth||!indices||!out||!counts||
       (uintptr_t)xy%_Alignof(float)||(uintptr_t)depth%_Alignof(float)||
       (uintptr_t)indices%_Alignof(uint16_t)||(uintptr_t)out%_Alignof(uint16_t)||
       (uintptr_t)counts%_Alignof(uint32_t)||
       (order!=AYATSURI_VISIBILITY_AUTHORED&&order!=AYATSURI_VISIBILITY_CENTROID_DEPTH))
        return AYATSURI_VISIBILITY_INVALID;
    if(!vertices||vertices>AYATSURI_VISIBILITY_VERTICES||!triangles||
       triangles>AYATSURI_VISIBILITY_TRIANGLES||capacity<3*triangles||capacity>SIZE_MAX/sizeof *out)
        return AYATSURI_VISIBILITY_RANGE;
    visibility_span input[3],output[2];
    if(!visibility_range(xy,2*vertices*sizeof *xy,input)||
       !visibility_range(depth,vertices*sizeof *depth,input+1)||
       !visibility_range(indices,3*triangles*sizeof *indices,input+2)||
       !visibility_range(out,capacity*sizeof *out,output)||
       !visibility_range(counts,3*sizeof *counts,output+1))return AYATSURI_VISIBILITY_RANGE;
    if(visibility_overlap(output[0],output[1]))return AYATSURI_VISIBILITY_OVERLAP;
    for(unsigned i=0;i<2;i++)for(unsigned j=0;j<3;j++)
        if(visibility_overlap(output[i],input[j]))return AYATSURI_VISIBILITY_OVERLAP;
    /* Validate even culled/unreferenced input, before any observable write. */
    for(size_t i=0;i<vertices;i++)
        if(!isfinite(xy[2*i])||!isfinite(xy[2*i+1])||fabsf(xy[2*i])>4096||
           fabsf(xy[2*i+1])>4096||!isfinite(depth[i]))return AYATSURI_VISIBILITY_INVALID;
    for(size_t i=0;i<3*triangles;i++)if(indices[i]>=vertices)return AYATSURI_VISIBILITY_RANGE;

    uint8_t ids[AYATSURI_VISIBILITY_TRIANGLES];
    double keys[AYATSURI_VISIBILITY_TRIANGLES];
    uint32_t result[3]={0,0,0};
    for(size_t triangle=0;triangle<triangles;triangle++){
        const uint16_t *v=indices+3*triangle;
        const double ax=xy[2*v[0]],ay=xy[2*v[0]+1],
            bx=xy[2*v[1]],by=xy[2*v[1]+1],cx=xy[2*v[2]],cy=xy[2*v[2]+1],
            area=(bx-ax)*(cy-ay)-(by-ay)*(cx-ax);
        if(area>1e-5){
            const double key=(double)depth[v[0]]+depth[v[1]]+depth[v[2]];
            unsigned at=result[0]++;
            /* Stable insertion keeps source-ID ties without changing winding. */
            if(order==AYATSURI_VISIBILITY_CENTROID_DEPTH)
                while(at&&keys[at-1]>key){keys[at]=keys[at-1];ids[at]=ids[at-1];--at;}
            keys[at]=key;ids[at]=(uint8_t)triangle;
        }else ++result[area < -1e-5?1:2];
    }
    /* Nothing below can fail; disjoint output cannot corrupt unread indices. */
    for(unsigned i=0;i<result[0];i++)memcpy(out+3*i,indices+3*ids[i],3*sizeof *out);
    memcpy(counts,result,sizeof result);return AYATSURI_VISIBILITY_OK;
}
