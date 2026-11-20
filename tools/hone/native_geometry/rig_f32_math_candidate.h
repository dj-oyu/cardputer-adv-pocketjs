#ifndef HONE_RIG_F32_MATH_CANDIDATE_H
#define HONE_RIG_F32_MATH_CANDIDATE_H
#include "affine_f32_candidate.h"
#include <stdint.h>
#include <stddef.h>
/* Independent, opt-in host math only. NOT a full rig evaluator or provider.
 * Same ordered-binary32 compiler requirements as affine_f32_candidate.h.
 * All spans are complete and immutable during the call. Public outputs commit
 * only on success; float outputs may overlap float input spans. uint32 index
 * arrays must be disjoint from float spans; do not cast between array types.
 * No pointers are kept. */
static inline int hone_rig_f32_euler_xyz(const float angles[3],float out[9]) {
 if(!angles||!out)return 0;
 for(unsigned k=0;k<3;k++)if(!isfinite(angles[k]))return 0;
 const double x=angles[0],y=angles[1],z=angles[2];
 const double cx=cos(x),sx=sin(x),cy=cos(y),sy=sin(y),cz=cos(z),sz=sin(z);
 const float result[9]={(float)(cz*cy),(float)(cz*sy*sx-sz*cx),(float)(cz*sy*cx+sz*sx),
  (float)(sz*cy),(float)(sz*sy*sx+cz*cx),(float)(sz*sy*cx-cz*sx),
  (float)-sy,(float)(cy*sx),(float)(cy*cx)};
 for(unsigned k=0;k<9;k++)if(!isfinite(result[k]))return 0;
 memcpy(out,result,sizeof result);return 1;
}
/* Row-major proper-rigid 3x3 -> wxyz. The identified branch admits ONLY a
 * positive normalized matrix trace AND nonnegative diagonal entries. This
 * conservative branch domain is explicit; other valid rotations fail;
 * callers must not silently use identity or another unverified branch.
 * Column normalization precedes conversion; do not re-normalize its result
 * merely to force bitwise unit length. This does not admit scale or shear. */
static inline int hone_rig_f32_matrix_quat_positive_trace(const float matrix[9],float out[4]) {
 if(!matrix||!out)return 0;
 float affine[12]={0};
 for(unsigned r=0;r<3;r++)for(unsigned k=0;k<3;k++)affine[4*r+k]=matrix[3*r+k];
 if(!hone_affine_f32_rigid(affine))return 0;
 float m[9];memcpy(m,matrix,sizeof m);
 for(unsigned k=0;k<3;k++) {
  const float length=sqrtf((m[k]*m[k]+m[3+k]*m[3+k])+m[6+k]*m[6+k]);
  if(!isfinite(length)||length<=0)return 0;
  const float reciprocal=1.0f/length;
  for(unsigned r=0;r<3;r++)m[3*r+k]*=reciprocal;
 }
 if(m[0]<0||m[4]<0||m[8]<0)return 0;
 const float trace=((1.0f+m[0])+m[4])+m[8];
 if(!isfinite(trace)||trace<=1.0f)return 0;
 const float root=sqrtf(trace),reciprocal=1.0f/(2.0f*root);
 const float result[4]={.5f*root,(m[7]-m[5])*reciprocal,
                       (m[2]-m[6])*reciprocal,(m[3]-m[1])*reciprocal};
 for(unsigned k=0;k<4;k++)if(!isfinite(result[k]))return 0;
 memcpy(out,result,sizeof result);return 1;
}
static inline void hone_dq_f32_mul_(const float a[4],const float b[4],float out[4]) {
 out[0]=a[0]*b[0]-a[1]*b[1]-a[2]*b[2]-a[3]*b[3];
 out[1]=a[0]*b[1]+a[1]*b[0]+a[2]*b[3]-a[3]*b[2];
 out[2]=a[0]*b[2]-a[1]*b[3]+a[2]*b[0]+a[3]*b[1];
 out[3]=a[0]*b[3]+a[1]*b[2]-a[2]*b[1]+a[3]*b[0];
}
/* Proper-rigid joint palette, seven floats per joint: wxyz, translation xyz.
 * Point and palette already share one coordinate space. Four influences max;
 * index/weight validation includes zero weights; unused joint values are not
 * inspected. Positive-weight quaternions must have squared length within 1e-6
 * of one. Quaternion hemisphere is chosen from the first nonzero influence.
 * No influence copies the point. Zero/underflowed blended norm and any overflow
 * fail. The homogeneous DQ formula divides by real norm squared once at the
 * end, rather than constructing a normalized quaternion first. No hemisphere,
 * binding, space transform, driver or model-lifetime admission is inferred. */
static inline int hone_dq_f32_position(const float point[3],const float *joints,size_t joint_count,
 const uint32_t *indices,const float *weights,size_t influences,float out[3]) {
 if(!point||!joints||!indices||!weights||!out||!joint_count||joint_count>65536||influences>4)return 0;
 for(unsigned k=0;k<3;k++)if(!isfinite(point[k]))return 0;
 float real[4]={0},dual[4]={0},reference[4]={0};int have=0;
 for(size_t n=0;n<influences;n++) {
  if(indices[n]>=joint_count||!isfinite(weights[n])||weights[n]<0)return 0;
  if(weights[n]==0)continue;
  const float *q=joints+7*(size_t)indices[n];float norm=0;
  for(unsigned k=0;k<7;k++)if(!isfinite(q[k]))return 0;
  for(unsigned k=0;k<4;k++)norm+=q[k]*q[k];
  if(!isfinite(norm)||fabsf(norm-1.0f)>1e-6f)return 0;
  if(!have){memcpy(reference,q,sizeof reference);have=1;}
  float dot=0;for(unsigned k=0;k<4;k++)dot+=reference[k]*q[k];
  const float weight=weights[n]*(dot<0?-1.0f:1.0f);
  const float translation[4]={0,q[4],q[5],q[6]};float product[4];
  hone_dq_f32_mul_(translation,q,product);
  for(unsigned k=0;k<4;k++){real[k]+=weight*q[k];dual[k]+=.5f*weight*product[k];}
 }
 float result[3];
 if(!have)memcpy(result,point,sizeof result);
 else {
  float norm2=0;for(unsigned k=0;k<4;k++)norm2+=real[k]*real[k];
  if(!isfinite(norm2)||norm2<=0)return 0;
  const float reciprocal=1.0f/norm2;
  if(!isfinite(reciprocal))return 0;
  const float w=real[0],x=real[1],y=real[2],z=real[3];
  const float m[9]={((w*w+x*x)-y*y)-z*z,2*(x*y-w*z),2*(x*z+w*y),
                   2*(x*y+w*z),((w*w-x*x)+y*y)-z*z,2*(y*z-w*x),
                   2*(x*z-w*y),2*(y*z+w*x),((w*w-x*x)-y*y)+z*z};
  const float conjugate[4]={w,-x,-y,-z};float translation[4];
  hone_dq_f32_mul_(dual,conjugate,translation);
  for(unsigned r=0;r<3;r++) {
   result[r]=(((m[3*r]*point[0]+m[3*r+1]*point[1])+m[3*r+2]*point[2])+2*translation[r+1])*reciprocal;
   if(!isfinite(result[r]))return 0;
  }
 }
 memcpy(out,result,sizeof result);return 1;
}
#endif
