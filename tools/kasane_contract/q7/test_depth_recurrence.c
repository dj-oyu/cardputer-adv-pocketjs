/* Standalone interval-containment and environment differential probe. */
#include <assert.h>
#include <stdio.h>
#include "../../../main/ui/kasane/ksn_mesh_q7_certified.c"
static uint32_t state=1701;
static uint32_t rng(void){state=1664525*state+1013904223;return state;}
static double operand(void){return ldexp((double)((int)(rng()%2049)-1024),(int)(rng()%121)-100);}
static void start_flags(void){feclearexcept(FE_ALL_EXCEPT);volatile double a=1,b=3,c=a/b;(void)c;assert(fetestexcept(FE_INEXACT));}
int main(void){unsigned admitted=0,checked=0;
 ksn_mesh_q7_certificate gate={0};gate.p[0][0]=0.125;gate.p[0][2]=400;gate.error[0]=1e-8;
 feclearexcept(FE_ALL_EXCEPT);assert(!recurrence_prepare(&gate,0,239,0).valid);assert(!fetestexcept(FE_ALL_EXCEPT));
 start_flags();assert(recurrence_prepare(&gate,0,239,0).valid);
 gate.error[0]=-1;assert(!recurrence_prepare(&gate,0,239,0).valid);
 gate.error[0]=1e8;assert(!recurrence_prepare(&gate,0,239,0).valid);
 gate.error[0]=1e-8;gate.p[0][0]=0x1p-101;assert(!recurrence_prepare(&gate,0,239,0).valid);
 gate.p[0][0]=0x1p31;assert(!recurrence_prepare(&gate,0,239,0).valid);
 gate.p[0][0]=1;gate.p[0][2]=-100;assert(!recurrence_prepare(&gate,0,239,0).valid);
 assert(fetestexcept(FE_ALL_EXCEPT)==FE_INEXACT);
 for(unsigned row=0;row<20000;row++){
  ksn_mesh_q7_certificate c={0};c.p[0][0]=operand();c.p[0][2]=operand();double ry=operand();
  double m=fabs(c.p[0][0])*239+fabs(ry)+fabs(c.p[0][2])+1;c.error[0]=m*128*DBL_EPSILON;
  int lo=(int)(rng()%120),hi=lo+(int)(rng()%(240-lo));
  start_flags();q7_depth_recurrence r=recurrence_prepare(&c,lo,hi,ry);assert(fetestexcept(FE_ALL_EXCEPT)==FE_INEXACT);
  if(!r.valid)continue;
  admitted++;
  for(int x=lo;x<=hi;x++){
   double v=(c.p[0][0]*x+ry)+c.p[0][2];
   assert(r.value-r.error<=v-c.error[0]&&r.value+r.error>=v+c.error[0]);
   float a=0,b=0;start_flags();bool direct=certified_depth(&c,x,ry,&a);int flags=fetestexcept(FE_ALL_EXCEPT);
   start_flags();bool fast=recurrence_get(&r,&b);assert(fetestexcept(FE_ALL_EXCEPT)==flags);
   if(fast){assert(direct&&!memcmp(&a,&b,sizeof a));}checked++;
   if(x<hi)r.value+=r.step;
  }
 }
 assert(admitted>1000&&checked>100000);printf("RECURRENCE_INTERVAL_PASS admitted=%u pixels=%u\n",admitted,checked);
}
