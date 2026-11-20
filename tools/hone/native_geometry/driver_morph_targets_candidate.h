#ifndef HONE_DRIVER_MORPH_TARGETS_CANDIDATE_H
#define HONE_DRIVER_MORPH_TARGETS_CANDIDATE_H
#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#if defined(__FAST_MATH__) || (defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__ > 0)
#error "morph targets require IEEE finite checks"
#endif
/* Complete, immutable, disjoint inputs and caller-owned output. Only source
 * blocks explicitly admitted by the emitter are routed. All outputs validate
 * before any copy; failure leaves the entire output unchanged. The caller
 * supplies masks and ensures the rest of the frame succeeds before publication. */
typedef struct {uint16_t program;uint32_t target;} hone_driver_morph_route;
static inline int hone_driver_morph_targets(const hone_driver_morph_route *routes,size_t route_count,
    const double *values,size_t value_count,const float *defaults,size_t target_count,float *out){
 if(route_count>4096||target_count>1000000||!defaults||!out||(route_count&&(!routes||!values)))return 0;
 for(size_t i=0;i<target_count;i++)if(!isfinite(defaults[i]))return 0;
 for(size_t i=0;i<route_count;i++){
  hone_driver_morph_route r=routes[i];
  if(r.program>=value_count||r.target>=target_count||!isfinite(values[r.program])||fabs(values[r.program])>FLT_MAX)return 0;
  for(size_t j=0;j<i;j++)if(routes[j].target==r.target)return 0;
 }
 memcpy(out,defaults,target_count*sizeof(*out));
 for(size_t i=0;i<route_count;i++)out[routes[i].target]=(float)values[routes[i].program];
 return 1;
}
#endif
