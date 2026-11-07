#include <assert.h>
#include <stdio.h>
#include "../../../main/ui/kasane/ksn_mesh_q7_certified.c"
static uint32_t seed=971;
static uint32_t rng(void){seed=1664525u*seed+1013904223u;return seed;}
static void check_conversion(uint64_t raw){
 union {uint64_t u;double d;} value={.u=raw};uint32_t bits,rem;
 assert(normal_float_bits(value.d,&bits,&rem));
 volatile double input=value.d;union{float f;uint32_t u;} expected={.f=(float)input};assert(bits==expected.u);
 assert((rem!=0)==((raw&UINT64_C(0x1fffffff))!=0));
}
int main(void){assert(fesetround(FE_TONEAREST)==0);unsigned conversions=0,pairs=0;
 const uint32_t tails[]={0,1,0xfffffff,0x10000000,0x10000001,0x1fffffff};
 for(unsigned exponent=898;exponent<=1149;exponent++)for(unsigned sign=0;sign<2;sign++)for(unsigned parity=0;parity<2;parity++)for(unsigned t=0;t<6;t++){
  uint64_t raw=((uint64_t)sign<<63)|((uint64_t)exponent<<52)|((uint64_t)parity<<29)|tails[t];check_conversion(raw);conversions++;
  raw|=((UINT64_C(1)<<52)-(UINT64_C(1)<<29));check_conversion(raw);conversions++;
 }
 for(unsigned i=0;i<200000;i++){
  uint64_t raw=((uint64_t)(rng()&1)<<63)|((uint64_t)(898+rng()%252)<<52)|((uint64_t)(rng()&0xfffff)<<32)|rng();check_conversion(raw);conversions++;
 }
 for(unsigned i=0;i<100000;i++){
  ksn_mesh_q7_certificate c={0};union{uint64_t u;double d;} value={.u=((uint64_t)(rng()&1)<<63)|((uint64_t)(1+rng()%2046)<<52)|((uint64_t)(rng()&0xfffff)<<32)|rng()};
  c.p[0][2]=value.d;c.error[0]=ldexp((double)(rng()%1024),(int)(rng()%2040)-1070);
  for(unsigned sticky=0;sticky<2;sticky++){
   float a=123,b=123;feclearexcept(FE_ALL_EXCEPT);if(sticky)feraiseexcept(FE_INEXACT);
   bool ok_a=certified_depth(&c,0,0,&a);int flags_a=fetestexcept(FE_ALL_EXCEPT);
   feclearexcept(FE_ALL_EXCEPT);if(sticky)feraiseexcept(FE_INEXACT);
   bool ok_b=certified_depth_once(&c,0,0,&b);int flags_b=fetestexcept(FE_ALL_EXCEPT);
   assert(ok_a==ok_b&&!memcmp(&a,&b,sizeof a)&&flags_a==flags_b);pairs++;
  }
 }
 const double values[]={400+0x1p-25,400-0x1p-25,400,400+0x1p-25,400+0x1p-16,0,0x1p-149,INFINITY,NAN};
 const double errors[]={0x1p-25,0x1p-25,0,0x1p-27,0x1p-40,0,0,0,0};
 const int sticky_flags[]={0,FE_INEXACT,FE_UNDERFLOW,FE_OVERFLOW,FE_INVALID,FE_DIVBYZERO,FE_ALL_EXCEPT};
 for(unsigned i=0;i<sizeof values/sizeof values[0];i++)for(unsigned j=0;j<sizeof sticky_flags/sizeof sticky_flags[0];j++){
  ksn_mesh_q7_certificate c={0};c.p[0][2]=values[i];c.error[0]=errors[i];float a=123,b=123;
  feclearexcept(FE_ALL_EXCEPT);feraiseexcept(sticky_flags[j]);bool ar=certified_depth(&c,0,0,&a);int af=fetestexcept(FE_ALL_EXCEPT);
  feclearexcept(FE_ALL_EXCEPT);feraiseexcept(sticky_flags[j]);bool br=certified_depth_once(&c,0,0,&b);int bf=fetestexcept(FE_ALL_EXCEPT);
  assert(ar==br&&!memcmp(&a,&b,sizeof a)&&af==bf);pairs++;
 }
 const double excluded[]={0,0x1p-149,0x1p-126,0x1p127,INFINITY,NAN};
 for(unsigned i=0;i<sizeof excluded/sizeof excluded[0];i++){uint32_t b,r;assert(!normal_float_bits(excluded[i],&b,&r));}
 printf("ROUNDED_BOUNDS_PASS conversions=%u ties/carries/signs/exponents; pairs=%u output/status/flags exact\n",conversions,pairs);
}
