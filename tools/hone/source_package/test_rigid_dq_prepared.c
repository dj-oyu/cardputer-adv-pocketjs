#include "rigid_dq_candidate.h"
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static uint32_t seed=7931;
static double sample(void){seed=1664525u*seed+1013904223u;return (double)(seed>>8)/16777216.0;}
static void compare(const double p[3],const double *raw,const rigid_dq_joint *ready,
                    const unsigned *index,const double *weight,size_t n){
 double a[3]={7,8,9},b[3]={7,8,9};
 int x=rigid_dq_position(p,raw,4,index,weight,n,a);
 int y=rigid_dq_position_prepared(p,ready,4,index,weight,n,b);
 assert(x==y);assert(memcmp(a,b,sizeof a)==0);
#ifdef DQ_FROZEN_ORACLE
 extern int frozen_position(const double *,const double *,size_t,const unsigned *,const double *,size_t,double *);
 double c[3]={7,8,9};int z=frozen_position(p,raw,4,index,weight,n,c);
 assert(x==z);assert(memcmp(a,c,sizeof a)==0);
#endif
}
int main(void){
 for(unsigned trial=0;trial<10000;trial++){
  double raw[28],p[3],w[4];rigid_dq_joint ready[4];unsigned ix[4];
  for(unsigned j=0;j<4;j++){
   double *q=raw+7*j,n=0;
   for(unsigned k=0;k<4;k++){q[k]=sample()*2-1;n+=q[k]*q[k];}
   n=sqrt(n);for(unsigned k=0;k<4;k++)q[k]/=n;
   for(unsigned k=4;k<7;k++)q[k]=(sample()*2-1)*100;
   assert(rigid_dq_prepare_joint(q,ready+j));
   ix[j]=(unsigned)(sample()*4);w[j]=trial%11==0?0:sample();
  }
  for(unsigned k=0;k<3;k++)p[k]=(sample()*2-1)*100;
  for(size_t n=0;n<=4;n++)compare(p,raw,ready,ix,w,n);
  ix[3]=4;compare(p,raw,ready,ix,w,4);ix[3]=0;
  w[3]=NAN;compare(p,raw,ready,ix,w,4);
  w[3]=-1;compare(p,raw,ready,ix,w,4);
  w[3]=INFINITY;compare(p,raw,ready,ix,w,4);
  w[3]=0;p[2]=NAN;compare(p,raw,ready,ix,w,4);
 }
 rigid_dq_joint out,saved;memset(&out,0x5a,sizeof out);saved=out;
 double bad[7]={2,0,0,0,0,0,0};
 assert(!rigid_dq_prepare_joint(bad,&out));assert(!memcmp(&out,&saved,sizeof out));
 bad[0]=1;bad[6]=NAN;assert(!rigid_dq_prepare_joint(bad,&out));assert(!memcmp(&out,&saved,sizeof out));
 assert(!rigid_dq_prepare_joint(NULL,&out));assert(!rigid_dq_prepare_joint(bad,NULL));
 bad[6]=0;assert(rigid_dq_prepare_joint(bad,&out));
 /* Antipodal representations and finite extreme translations keep the same
  * output/failure behavior; preparation does not normalize/reorder them. */
 double raw[28]={1,0,0,0,DBL_MAX,DBL_MAX,DBL_MAX,-1,0,0,0,DBL_MAX,DBL_MAX,DBL_MAX,
                 1,0,0,0,0,0,0,1,0,0,0,0,0,0};
 rigid_dq_joint ready[4];for(unsigned j=0;j<4;j++)assert(rigid_dq_prepare_joint(raw+7*j,ready+j));
 unsigned ix[4]={0,1,2,3};double w[4]={.5,.5,0,0},p[3]={1,2,3};
 compare(p,raw,ready,ix,w,4);w[0]=DBL_MAX;compare(p,raw,ready,ix,w,4);
 for(unsigned k=0;k<4;k++)raw[k]=.5;
 assert(rigid_dq_prepare_joint(raw,ready));
 assert(!isfinite(ready[0].translation_product[0]));
 w[0]=1;w[1]=0;compare(p,raw,ready,ix,w,4);
 puts("PASS prepared DQ: 10000 palettes, exact output/status, late failures, retry and extreme inputs");
}
