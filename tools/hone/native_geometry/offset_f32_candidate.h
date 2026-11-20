#ifndef HONE_OFFSET_F32_CANDIDATE_H
#define HONE_OFFSET_F32_CANDIDATE_H
#include <float.h>
#include <math.h>
#include <string.h>
#if FLT_RADIX != 2 || FLT_MANT_DIG != 24
#error "offset candidate requires binary32 float"
#endif
#if defined(__FAST_MATH__) || (defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__ > 0)
#error "offset candidate requires finite checks and ordered operations"
#endif
/* Experimental black-box-derived ON_SURFACE arithmetic, not a nearest query.
 * All inputs share target-local space; normal is the selected triangle's unit
 * geometric normal. Caller owns selection/ties, topology, transforms and frame
 * lifetime. FP contraction must be off, round-to-nearest active. No allocation.
 * Failure leaves out unchanged. Any float input/output alias is supported.
 * No GPL implementation was used. See F32_FRAME_BOUNDARIES.md for limitations. */
static inline int hone_offset_f32(const float point[3], const float hit[3],
                                 const float normal[3], float offset, float out[3]) {
 if(!point||!hit||!normal||!out||!isfinite(offset))return 0;
 float d[3],n[3],h[3],candidate[3];
 for(unsigned k=0;k<3;k++){
  if(!isfinite(point[k])||!isfinite(hit[k])||!isfinite(normal[k]))return 0;
  h[k]=hit[k];n[k]=normal[k];d[k]=point[k]-hit[k];
  if(!isfinite(d[k]))return 0;
 }
 float norm2=(n[0]*n[0]+n[1]*n[1])+n[2]*n[2];
 if(!isfinite(norm2)||fabsf(norm2-1.f)>1.e-5f)return 0;
 float squared=(d[0]*d[0]+d[1]*d[1])+d[2]*d[2];
 if(!isfinite(squared))return 0;
 float length=sqrtf(squared);
 memcpy(candidate,h,sizeof candidate);
 if(length>=FLT_EPSILON && offset!=0.f){
  float dot=(d[0]*n[0]+d[1]*n[1])+d[2]*n[2];
  if(!isfinite(dot))return 0;
  if(dot<0.f)for(unsigned k=0;k<3;k++)n[k]=-n[k];
  float magnitude=((fabsf(h[0])+fabsf(h[1]))+fabsf(h[2]))+fabsf(offset);
  if(!isfinite(magnitude))return 0;
  /* The coordinate-dependent transition and the epsilon endpoint were
   * independently identified and tested; these are candidate semantics,
   * not an adjustable accuracy tolerance or coordinate snapping. */
  float transition=magnitude*1.e-4f;
  float weight=transition>0.f?fminf(1.f,length/transition):1.f;
  float inverse=1.f/length;
  for(unsigned k=0;k<3;k++){
   float direction=d[k]*inverse;
   float mixed=(1.f-weight)*n[k]+weight*direction;
   candidate[k]=h[k]+offset*mixed;
   if(!isfinite(candidate[k]))return 0;
  }
 }
 memcpy(out,candidate,sizeof candidate);return 1;
}
#endif
