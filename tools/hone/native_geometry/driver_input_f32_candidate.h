#ifndef HONE_DRIVER_INPUT_F32_CANDIDATE_H
#define HONE_DRIVER_INPUT_F32_CANDIDATE_H
#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#if FLT_RADIX != 2 || FLT_MANT_DIG != 24
#error "driver input candidate requires binary32 float"
#endif
#if defined(__FAST_MATH__) || (defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__ > 0)
#error "driver input candidate requires finite checks"
#endif
/* Opt-in adapter for a resolved input bundle explicitly admitted to the
 * binary32-variable contract. It does NOT change driver expression arithmetic,
 * bind property IDs, or assert that arbitrary input types have this contract.
 * Preflight all inputs before writing; failure preserves the destination.
 * Equal spans or disjoint double spans only. Input binding/order and lifetime
 * must remain unchanged until frame evaluation completes. */
static inline int hone_driver_input_f32(const double *input,size_t count,double *out){
 if(!count)return 1;
 if(!input||!out||count>SIZE_MAX/sizeof(double))return 0;
 size_t bytes=count*sizeof(double);uintptr_t a=(uintptr_t)input,b=(uintptr_t)out;
 if(bytes>UINTPTR_MAX-a||bytes>UINTPTR_MAX-b)return 0;
 if(a!=b && a<b+bytes && b<a+bytes)return 0;
 for(size_t k=0;k<count;k++)
  if(!isfinite(input[k])||fabs(input[k])>(double)FLT_MAX)return 0;
 for(size_t k=0;k<count;k++)out[k]=(double)(float)input[k];
 return 1;
}
#endif
