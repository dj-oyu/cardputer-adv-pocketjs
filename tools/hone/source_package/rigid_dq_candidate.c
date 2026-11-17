/* Independently written experimental rigid DQ position consumer.
 * Inputs: normalized proper-rotation quaternion wxyz + translation xyz for
 * each resolved joint. Scale/shear, driver evaluation, normals, publication
 * and renderer opacity are deliberately outside this candidate.
 * No allocation. Output is committed only on success for this one vertex.
 */
#include <math.h>
#include <stddef.h>
#include "rigid_dq_candidate.h"
#if defined(__FAST_MATH__) || (defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__ > 0)
#error "rigid DQ requires finite checks"
#endif
static void qm(const double*a,const double*b,double*r){
 r[0]=a[0]*b[0]-a[1]*b[1]-a[2]*b[2]-a[3]*b[3];
 r[1]=a[0]*b[1]+a[1]*b[0]+a[2]*b[3]-a[3]*b[2];
 r[2]=a[0]*b[2]-a[1]*b[3]+a[2]*b[0]+a[3]*b[1];
 r[3]=a[0]*b[3]+a[1]*b[2]-a[2]*b[1]+a[3]*b[0];
}
int rigid_dq_prepare_joint(const double joint[7],rigid_dq_joint *out){
 if(!joint||!out)return 0;
 double norm=0,t[4]={0,joint[4],joint[5],joint[6]};rigid_dq_joint result;
 for(int k=0;k<7;k++)if(!isfinite(joint[k]))return 0;
 for(int k=0;k<4;k++)norm+=joint[k]*joint[k];
 if(fabs(norm-1)>1e-6)return 0;
 for(int k=0;k<4;k++)result.rotation[k]=joint[k];
 qm(t,joint,result.translation_product);
 /* The original may overflow this product for finite translations. Retain
  * it: the vertex path must make the same final-result rejection. */
 *out=result;return 1;
}
static int position(const double *p,const double *joints,const rigid_dq_joint *prepared,size_t count,const unsigned *indices,const double *weights,size_t influences,double *out){
 if(!p||!out||(!joints&&!prepared)||!indices||!weights||!count||influences>4)return 0;
 for(int k=0;k<3;k++)if(!isfinite(p[k]))return 0;
 double a[4]={0},b[4]={0},ref[4]={0};int have=0;
 for(size_t i=0;i<influences;i++){
  if(indices[i]>=count||!isfinite(weights[i])||weights[i]<0)return 0;
  if(weights[i]==0)continue;
  const double*q;double d[4],dot=0;
  if(prepared){
   q=prepared[indices[i]].rotation;
   for(int k=0;k<4;k++)d[k]=prepared[indices[i]].translation_product[k];
  }else{
   q=joints+7*indices[i];double norm=0,t[4]={0,q[4],q[5],q[6]};
   for(int k=0;k<7;k++)if(!isfinite(q[k]))return 0;
   for(int k=0;k<4;k++)norm+=q[k]*q[k];
   if(fabs(norm-1)>1e-6)return 0;
   qm(t,q,d);
  }
  if(!have){for(int k=0;k<4;k++)ref[k]=q[k];have=1;}
  for(int k=0;k<4;k++)dot+=ref[k]*q[k];
  double w=weights[i]*(dot<0?-1:1);
  for(int k=0;k<4;k++){a[k]+=w*q[k];b[k]+=.5*w*d[k];}
 }
 double result[3];
 if(!have){for(int k=0;k<3;k++)result[k]=p[k];}
 else{
  double n=0,dot=0;for(int k=0;k<4;k++)n+=a[k]*a[k];n=sqrt(n);
  if(!isfinite(n)||n<1e-12)return 0;
  for(int k=0;k<4;k++){a[k]/=n;b[k]/=n;dot+=a[k]*b[k];}
  for(int k=0;k<4;k++)b[k]-=a[k]*dot;
  double c[4]={a[0],-a[1],-a[2],-a[3]},v[4]={0,p[0],p[1],p[2]},u[4],r[4],t[4];
  qm(a,v,u);qm(u,c,r);qm(b,c,t);
  for(int k=0;k<3;k++){result[k]=r[k+1]+2*t[k+1];if(!isfinite(result[k]))return 0;}
 }
 for(int k=0;k<3;k++)out[k]=result[k];
 return 1;
}
int rigid_dq_position(const double *p,const double *joints,size_t count,const unsigned *indices,const double *weights,size_t influences,double *out){
 return position(p,joints,NULL,count,indices,weights,influences,out);
}
int rigid_dq_position_prepared(const double *p,const rigid_dq_joint *joints,size_t count,const unsigned *indices,const double *weights,size_t influences,double *out){
 return position(p,NULL,joints,count,indices,weights,influences,out);
}
