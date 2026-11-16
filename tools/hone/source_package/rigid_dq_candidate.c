/* Independently written experimental rigid DQ position consumer.
 * Inputs: normalized proper-rotation quaternion wxyz + translation xyz for
 * each resolved joint. Scale/shear, driver evaluation, normals, publication
 * and renderer opacity are deliberately outside this candidate.
 * No allocation. Output is committed only on success for this one vertex.
 */
#include <math.h>
#include <stddef.h>
static void qm(const double*a,const double*b,double*r){
 r[0]=a[0]*b[0]-a[1]*b[1]-a[2]*b[2]-a[3]*b[3];
 r[1]=a[0]*b[1]+a[1]*b[0]+a[2]*b[3]-a[3]*b[2];
 r[2]=a[0]*b[2]-a[1]*b[3]+a[2]*b[0]+a[3]*b[1];
 r[3]=a[0]*b[3]+a[1]*b[2]-a[2]*b[1]+a[3]*b[0];
}
int rigid_dq_position(const double *p,const double *joints,size_t count,const unsigned *indices,const double *weights,size_t influences,double *out){
 if(!p||!out||!joints||!indices||!weights||!count||influences>4)return 0;
 for(int k=0;k<3;k++)if(!isfinite(p[k]))return 0;
 double a[4]={0},b[4]={0},ref[4]={0};int have=0;
 for(size_t i=0;i<influences;i++){
  if(indices[i]>=count||!isfinite(weights[i])||weights[i]<0)return 0;
  if(weights[i]==0)continue;
  const double*q=joints+7*indices[i];double norm=0,d[4],t[4]={0,q[4],q[5],q[6]},dot=0;
  for(int k=0;k<7;k++)if(!isfinite(q[k]))return 0;
  for(int k=0;k<4;k++)norm+=q[k]*q[k];
  if(fabs(norm-1)>1e-6)return 0;
  if(!have){for(int k=0;k<4;k++)ref[k]=q[k];have=1;}
  for(int k=0;k<4;k++)dot+=ref[k]*q[k];
  qm(t,q,d);double w=weights[i]*(dot<0?-1:1);
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
