#include <assert.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <fenv.h>
#include "ui/kasane/ksn_mesh_q7_certified.h"
bool old_q7_render(const ksn_mesh_q7_snapshot*,uint16_t*,unsigned,unsigned,float*,uint8_t*,unsigned,ksn_mesh_q7_stats*);
int main(void){
 unsigned char bytes[4096];ksn_mesh_snapshot source={.packets={bytes,sizeof bytes,0,0},.enabled=true};
 co_vertex v[3]={{0,0,-1,1,0xf800,0x07e0},{16,0,-1,1,0xf800,0x07e0},{0,16,-1,1,0xf800,0x07e0}};
 uint16_t ix[3]={0,1,2};co_input in={v,ix,0,3,1};assert(co_append(&source.packets,&in));
 size_t pb=sizeof(ksn_mesh_prepared)+sizeof(ksn_prepared_triangle),qb=sizeof(ksn_mesh_q7_snapshot)+sizeof(ksn_mesh_q7_triangle);
 ksn_mesh_prepared*p=malloc(pb);ksn_mesh_q7_snapshot*q=malloc(qb);assert(p&&q);
 for(unsigned failure=0;failure<2;failure++)for(unsigned fallback=0;fallback<2;fallback++){
  assert(ksn_mesh_prepare(&source,p,pb)&&ksn_mesh_q7_prepare(p,q,qb));q->fallback=fallback;
  if(failure){q->triangles[0].cert.valid=false;q->triangles[0].original.v[0].depth=INFINITY;}
  uint16_t pixels[2][240*8]={{0}};float z[2][240*8]={{0}};uint8_t owners[2][240*8]={{0}};
  ksn_mesh_q7_stats stats[2];memset(stats,0xff,sizeof stats);bool ok[2];int flags[2];
  for(unsigned arm=0;arm<2;arm++){
   feclearexcept(FE_ALL_EXCEPT);
   ok[arm]=arm?ksn_mesh_q7_render(q,pixels[arm],1,8,z[arm],owners[arm],8,&stats[arm]):old_q7_render(q,pixels[arm],1,8,z[arm],owners[arm],8,&stats[arm]);
   flags[arm]=fetestexcept(FE_ALL_EXCEPT);
  }
  assert(ok[0]==!failure&&ok[0]==ok[1]&&flags[0]==flags[1]);
  assert(!memcmp(pixels[0],pixels[1],sizeof pixels[0])&&!memcmp(z[0],z[1],sizeof z[0])&&!memcmp(owners[0],owners[1],sizeof owners[0])&&!memcmp(&stats[0],&stats[1],sizeof stats[0]));
 }
 free(p);free(q);puts("PASS uint64 wrap and nonfinite partial-return; original/Q7 pixels depth owner counters flags exact");
}
