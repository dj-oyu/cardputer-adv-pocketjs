/* Standalone static-helper differential, generic coefficients only. */
#include "../../../main/ui/kasane/ksn_mesh_q7_certified.c"
#include <assert.h>
#include <stdio.h>
static uint32_t rng=17;
static uint32_t next(void){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
int main(void){
 unsigned successes=0,failures=0;
 assert(fesetround(FE_TONEAREST)==0);
 for(unsigned trial=0;trial<2400;trial++){
  ksn_mesh_q7_certificate c={0};c.valid=true;
  c.p[0][0]=ldexp((int32_t)next(),(int)(next()%250)-160);
  c.p[0][1]=trial&1?-0.:0.;
  c.p[0][2]=ldexp((int32_t)next(),(int)(next()%250)-160);
  c.error[0]=trial%7?ldexp(1.,(int)(next()%250)-180):0.;
  if(trial%11==0)c.p[0][2]=trial%22?INFINITY:NAN;
  float result[8*240];bool ok[8*240];int flags[8*240];
  feclearexcept(FE_ALL_EXCEPT);if(trial&2)feraiseexcept(FE_ALL_EXCEPT);
  for(unsigned row=0;row<8;row++)for(unsigned x=0;x<240;x++){
   unsigned i=row*240+x;result[i]=0;
   ok[i]=certified_depth(&c,(int)x,c.p[0][1]*row,&result[i]);
   flags[i]=fetestexcept(FE_ALL_EXCEPT);
  }
  q7_depth_cache cache={0};
  feclearexcept(FE_ALL_EXCEPT);if(trial&2)feraiseexcept(FE_ALL_EXCEPT);
  for(unsigned row=0;row<8;row++)for(unsigned x=0;x<240;x++){
   unsigned i=row*240+x;float value=0;
   bool got=cached_depth(&c,(int)x,c.p[0][1]*row,&value,&cache);
   assert(got==ok[i]&&fetestexcept(FE_ALL_EXCEPT)==flags[i]);
   assert(!memcmp(&value,&result[i],sizeof value));
   successes+=got;failures+=!got;
  }
 }
 printf("DEPTH_CACHE_HELPER_PASS operations=%u successful=%u failed=%u flags_after_each_equal\n",successes+failures,successes,failures);
}
