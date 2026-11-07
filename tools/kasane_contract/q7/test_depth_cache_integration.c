#include "ui/kasane/ksn_mesh_q7_certified.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fenv.h>
#include <math.h>
bool old_q7_render(const ksn_mesh_q7_snapshot*,uint16_t*,unsigned,unsigned,float*,uint8_t*,unsigned,ksn_mesh_q7_stats*);
static unsigned char bytes[8192];static uint16_t image[2][240*135];static float z[2][240*8];static uint8_t owner[2][240*8];
int main(void){
 size_t pb=sizeof(ksn_mesh_prepared)+32*sizeof(ksn_prepared_triangle),qb=sizeof(ksn_mesh_q7_snapshot)+32*sizeof(ksn_mesh_q7_triangle);
 ksn_mesh_prepared*p=malloc(pb);ksn_mesh_q7_snapshot*q=malloc(qb);assert(p&&q);unsigned comparisons=0;
 const int rounds[]={FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO};const unsigned heights[]={1,4,8,135};
 for(unsigned variant=0;variant<8;variant++){
  assert(!fesetround(FE_TONEAREST));ksn_mesh_snapshot s={.packets={bytes,sizeof bytes,0,0},.enabled=true};
  for(unsigned f=0;f<16;f++){
   float top=f<2?100:0,depth=(float)f;
   co_vertex v[3]={{0,top,-1,depth,0xf800,0x07e0},{239,top,-1,depth+1,0xf800,0x07e0},{0,134,-1,depth,0xf800,0x07e0}};
   uint16_t ix[3]={0,1,2};co_input in={v,ix,(uint16_t)f,3,1};assert(co_append(&s.packets,&in));
  }
  assert(ksn_mesh_prepare(&s,p,pb)&&ksn_mesh_q7_prepare(p,q,qb)&&q->count==16&&!q->fallback);
  for(unsigned f=0;f<16;f++){
   assert(q->triangles[f].cert.valid);q->triangles[f].cert.p[0][1]=(f&1)?-0.0:0.0;
   if(variant&1)if(f%3==0)q->triangles[f].cert.p[0][1]=0x1p-100;
   if(variant&2)if(f%4==1)q->triangles[f].cert.valid=false;
  }
  if(variant&4)q->fallback=true;
  for(unsigned round=0;round<4;round++)for(unsigned h=0;h<4;h++)for(unsigned rows=1;rows<=8;rows+=7){
   assert(!fesetround(rounds[round]));ksn_mesh_q7_stats st[2];memset(st,0xff,sizeof st);
   memset(image,0,sizeof image);
   for(unsigned y=0;y<135;y+=heights[h]){
    unsigned n=heights[h];if(n>135-y)n=135-y;int flags[2];bool result[2];
    for(unsigned arm=0;arm<2;arm++){
     memset(z[arm],0,sizeof z[arm]);memset(owner[arm],0,sizeof owner[arm]);feclearexcept(FE_ALL_EXCEPT);
     if((y+h)&1)feraiseexcept(FE_INVALID|FE_DIVBYZERO);
     result[arm]=arm?ksn_mesh_q7_render(q,image[arm]+y*240,y,n,z[arm],owner[arm],rows,&st[arm]):old_q7_render(q,image[arm]+y*240,y,n,z[arm],owner[arm],rows,&st[arm]);
     flags[arm]=fetestexcept(FE_ALL_EXCEPT);
    }
    assert(result[0]==result[1]&&result[0]);assert(flags[0]==flags[1]);assert(!memcmp(z[0],z[1],sizeof z[0]));assert(!memcmp(owner[0],owner[1],sizeof owner[0]));
    assert(!memcmp(image[0],image[1],sizeof image[0]));assert(!memcmp(&st[0],&st[1],sizeof st[0]));comparisons++;
   }
  }
 }
 assert(!fesetround(FE_TONEAREST));free(p);free(q);printf("CACHE_INTEGRATION_PASS comparisons=%u faces16 skipped_bands slopes_positive_negative_zero_tiny invalid_cert fallback all_rounding rows1_8 heights1_4_8_135 cleared_sticky_flags_per_call counter_wrap\n",comparisons);
}
