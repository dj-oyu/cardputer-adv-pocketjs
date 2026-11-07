#include <assert.h>
#include <stdio.h>
#include "../../../main/ui/kasane/ksn_mesh_q7_certified.c"
int main(void){
 const int modes[]={FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO};
 for(unsigned m=0;m<4;m++)for(unsigned sticky=0;sticky<2;sticky++){
  assert(fesetround(modes[m])==0);
  for(unsigned x=0;x<240;x++){
   feclearexcept(FE_ALL_EXCEPT);if(sticky)feraiseexcept(FE_INEXACT);
   volatile unsigned input=x;double expected=(double)input;int before=fetestexcept(FE_ALL_EXCEPT);
   double actual=screen_x_double(input);int after=fetestexcept(FE_ALL_EXCEPT);
   assert(!memcmp(&expected,&actual,sizeof actual)&&before==after&&fegetround()==modes[m]);
  }
 }
 assert(fesetround(FE_TONEAREST)==0);feclearexcept(FE_ALL_EXCEPT);
 puts("EXACT_X_PASS all240 coordinates,4 rounding modes,clear/sticky flags,bitwise binary64 equality");
}
