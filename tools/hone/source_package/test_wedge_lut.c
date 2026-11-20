#include <assert.h>
#include <math.h>
#include "native_wedge_lut_candidate.c"
int main(void){
 assert(measured_angle_weight(-1)==2 && measured_angle_weight(1)==0);
 assert(fabs(measured_angle_weight(-1e-10)-measured_angle_weight(1e-10))<1e-8);
 for(int i=1;i<64;i++){double k=i/64.;assert(fabs(measured_angle_weight(k-1e-9)-measured_angle_weight(k+1e-9))<1e-6);}
 double prev=2;for(int i=0;i<=20000;i++){double d=-1.+i/10000.;double w=measured_angle_weight(d);assert(isfinite(w)&&w>=0&&w<=2&&w<=prev+1e-12);prev=w;}
 return 0;}
