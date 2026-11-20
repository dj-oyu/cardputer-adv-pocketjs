#ifndef HONE_AFFINE_F32_CANDIDATE_H
#define HONE_AFFINE_F32_CANDIDATE_H
#include <float.h>
#include <math.h>
#include <string.h>
#if FLT_RADIX != 2 || FLT_MANT_DIG != 24 || FLT_EVAL_METHOD != 0
#error "affine candidate requires binary32 without excess precision"
#endif
#if defined(__FAST_MATH__) || (defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__ > 0)
#error "affine candidate requires finite checks and ordered arithmetic"
#endif
/* Independent host candidate, NOT wired into wrap/provider. Compile with
 * contraction disabled (-ffp-contract=off); no reassociation/fast-math.
 * Complete row-major 3x4 float spans, immutable for the call. Only proper-rigid
 * WORLD matrices are admitted; scale, shear, reflection and singular maps fail.
 * Relative means inverse(target_world)*source_world, then its inverse, with
 * explicit binary32 operation order. No parent hierarchy is inferred. */
static inline int hone_affine_f32_rigid(const float m[12]) {
 if(!m)return 0;
 for(unsigned k=0;k<12;k++)if(!isfinite(m[k]))return 0;
 for(unsigned i=0;i<3;i++)for(unsigned j=0;j<3;j++) {
  double dot=0;for(unsigned r=0;r<3;r++)dot+=(double)m[r*4+i]*m[r*4+j];
  if(fabs(dot-(i==j?1.0:0.0))>1e-5)return 0;
 }
 double det=(double)m[0]*((double)m[5]*m[10]-(double)m[6]*m[9])-
            (double)m[1]*((double)m[4]*m[10]-(double)m[6]*m[8])+
            (double)m[2]*((double)m[4]*m[9]-(double)m[5]*m[8]);
 return det>0 && fabs(det-1.0)<=2e-5;
}
/* Internal helpers may leave unpublished scratch changed on failure. The
 * cofactor/translation expansions are ordinary affine inverse identities;
 * their grouping was independently tested against synthetic black-box output. */
static inline int hone_affine_f32_inverse_(const float m[12],float out[12]) {
 const float a=m[0],b=m[1],c=m[2],x=m[3],d=m[4],e=m[5],f=m[6],y=m[7],
             g=m[8],h=m[9],i=m[10],z=m[11];
 const float co[9]={e*i-f*h,c*h-b*i,b*f-c*e,
                    f*g-d*i,a*i-c*g,c*d-a*f,
                    d*h-e*g,b*g-a*h,a*e-b*d};
 const float det=(c*co[6]+f*co[7])+i*co[8];
 if(!isfinite(det)||det<=0)return 0;
 const float reciprocal=1.0f/det;
 if(!isfinite(reciprocal))return 0;
 for(unsigned r=0;r<3;r++)for(unsigned k=0;k<3;k++)out[r*4+k]=co[r*3+k]*reciprocal;
 const float p=f*z-y*i,q=c*z-x*i,s=c*y-x*f;
 out[3]=-((b*p-e*q)+h*s)*reciprocal;
 out[7]=((a*p-d*q)+g*s)*reciprocal;
 out[11]=-((co[6]*x+co[7]*y)+co[8]*z)*reciprocal;
 for(unsigned k=0;k<12;k++)if(!isfinite(out[k]))return 0;
 return 1;
}
static inline int hone_affine_f32_mul_(const float a[12],const float b[12],float out[12]) {
 for(unsigned r=0;r<3;r++)for(unsigned k=0;k<4;k++) {
  const float p=a[r*4]*b[k],q=a[r*4+1]*b[4+k],s=a[r*4+2]*b[8+k];
  const float t=k==3?a[r*4+3]:0.0f;
  out[r*4+k]=(p+q)+(s+t);
  if(!isfinite(out[r*4+k]))return 0;
 }
 return 1;
}
/* out[0:12] forward, out[12:24] reverse. ALL output remains unchanged on
 * failure, including a reverse-stage overflow. Input/output overlap is allowed
 * for complete writable spans: input reads finish before the single commit.
 * The caller provides synchronization; this is not a cross-thread publication. */
static inline int hone_affine_f32_relative(const float target[12],const float source[12],float out[24]) {
 if(!out||!hone_affine_f32_rigid(target)||!hone_affine_f32_rigid(source))return 0;
 float target_inverse[12],result[24];
 if(!hone_affine_f32_inverse_(target,target_inverse)||
    !hone_affine_f32_mul_(target_inverse,source,result)||
    !hone_affine_f32_rigid(result)||
    !hone_affine_f32_inverse_(result,result+12)||
    !hone_affine_f32_rigid(result+12))return 0;
 memcpy(out,result,sizeof result);return 1;
}
/* Point application has the same failure/alias contract. The map must itself
 * be admitted proper-rigid. Finite inputs whose intermediate/final arithmetic
 * overflows are rejected; no clamping or coordinate snapping is performed. */
static inline int hone_affine_f32_point(const float map[12],const float point[3],float out[3]) {
 if(!point||!out||!hone_affine_f32_rigid(map))return 0;
 for(unsigned k=0;k<3;k++)if(!isfinite(point[k]))return 0;
 float result[3];
 for(unsigned r=0;r<3;r++) {
  result[r]=((map[r*4]*point[0]+map[r*4+1]*point[1])+map[r*4+2]*point[2])+map[r*4+3];
  if(!isfinite(result[r]))return 0;
 }
 memcpy(out,result,sizeof result);return 1;
}
#endif
