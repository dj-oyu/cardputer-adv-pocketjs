#define _POSIX_C_SOURCE 200809L
#include "ui/kasane/ksn_mesh_q7_certified.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fenv.h>
bool old_q7_render(const ksn_mesh_q7_snapshot*,uint16_t*,unsigned,unsigned,float*,uint8_t*,unsigned,ksn_mesh_q7_stats*);
static unsigned char bytes[4096];static uint16_t image[2][240*135];static float z[2][240*8];static uint8_t owner[2][240*8];
static uint64_t stamp(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return(uint64_t)t.tv_sec*1000000000u+t.tv_nsec;}
static void fixture(ksn_mesh_snapshot*s,unsigned f){
 s->packets=(co_buffer){bytes,sizeof bytes,0,0};s->enabled=true;
 const uint16_t colors[4]={0xf800,0x07e0,0x001f,0xffe0},ix[6]={0,1,2,0,2,3};
 for(unsigned k=0;k<4;k++){
  float d=k%2?4:-4,x=-24+16*(int)k+112+(f%16),y=-8+8*(int)k+64+((f>>4)%4);
  co_vertex v[4]={{x-40,y-28,-1,d+(int)k-402,colors[k],colors[k]},
    {x+40,y-28,-1,-d+(int)k-402,colors[k],colors[k]},
    {x+40,y+28,-1,-d+(int)k-402,colors[k],colors[k]},
    {x-40,y+28,-1,d+(int)k-402,colors[k],colors[k]}};
  co_input in={v,ix,(uint16_t)(k+1),4,2};assert(co_append(&s->packets,&in));
 }
}
static void corner_cases(ksn_mesh_prepared*p,size_t pb,ksn_mesh_q7_snapshot*q,size_t qb){
 for(unsigned scenario=0;scenario<9;scenario++){
  ksn_mesh_snapshot source={.packets={bytes,sizeof bytes,0,0},.enabled=true};
  for(unsigned face=0;face<2;face++){
   float mask=scenario==2?0:-1;float depth=scenario==3?(face?0:1):(float)face;
   co_vertex v[3]={{0,0,mask,depth,0xf800,0x07e0},{16,0,mask,depth,0xf800,0x07e0},{0,16,mask,depth,0xf800,0x07e0}};
   uint16_t ix[3]={0,1,2};co_input in={v,ix,(uint16_t)face,3,1};assert(co_append(&source.packets,&in));
  }
  assert(ksn_mesh_prepare(&source,p,pb)&&ksn_mesh_q7_prepare(p,q,qb));
  for(unsigned f=0;f<q->count;f++){
   if(scenario==1)q->triangles[f].cert.p[1][0]=-0.0;
   if(scenario==4){q->triangles[f].fixed.rgb[0][0][0]=1e-8;q->triangles[f].fixed.dx[0][0]=0;}
   if(scenario==5)q->triangles[f].fixed.dx[1][2]=1;
   if(scenario==6)q->triangles[f].cert.valid=false;
  }
  if(scenario==7)q->fallback=true;
  if(scenario==8)assert(fesetround(FE_DOWNWARD)==0);
  ksn_mesh_q7_stats stats[2]={{0}};
  for(unsigned y=0;y<135;y++){
   int flags[2];for(unsigned arm=0;arm<2;arm++){
    memset(image[arm],0,sizeof image[arm]);memset(z[arm],0,sizeof z[arm]);memset(owner[arm],0,sizeof owner[arm]);
    feclearexcept(FE_ALL_EXCEPT);
    bool ok=arm?ksn_mesh_q7_render(q,image[arm],y,1,z[arm],owner[arm],1,&stats[arm]):old_q7_render(q,image[arm],y,1,z[arm],owner[arm],1,&stats[arm]);
    flags[arm]=fetestexcept(FE_ALL_EXCEPT);assert(ok);
   }
   assert(flags[0]==flags[1]&&!memcmp(image[0],image[1],sizeof image[0])&&!memcmp(z[0],z[1],sizeof z[0])&&!memcmp(owner[0],owner[1],sizeof owner[0]));
  }
  assert(!memcmp(&stats[0],&stats[1],sizeof stats[0]));assert(fesetround(FE_TONEAREST)==0);
 }
 puts("ROW_REUSE_CASES_PASS plus_minus_zero uncertain_mask occlusion quantized_zero_step one_step invalid_certificate fallback rounding stats_fenv_equal");
}
int main(int argc,char**argv){unsigned band=argc>1?(unsigned)atoi(argv[1]):4,rows=argc>2?(unsigned)atoi(argv[2]):1;assert(band==4||band==8);assert(rows==1||rows==8);
 size_t pb=sizeof(ksn_mesh_prepared)+8*sizeof(ksn_prepared_triangle),qb=sizeof(ksn_mesh_q7_snapshot)+8*sizeof(ksn_mesh_q7_triangle);
 ksn_mesh_prepared*p=malloc(pb);ksn_mesh_q7_snapshot*q=malloc(qb);assert(p&&q);
 corner_cases(p,pb,q,qb);
 for(unsigned trial=0;trial<9;trial++){
  uint64_t ns[2]={0};uint32_t hash=2166136261u;ksn_mesh_q7_stats stats[2]={{0}};ksn_mesh_snapshot s={0};
  for(unsigned f=0;f<150;f++){
   fixture(&s,f);assert(ksn_mesh_prepare(&s,p,pb)&&ksn_mesh_q7_prepare(p,q,qb)&&!q->fallback);
   for(unsigned i=0;i<240*135;i++)image[0][i]=image[1][i]=0x0841;
   for(unsigned y=0;y<135;y+=band){unsigned n=135-y<band?135-y:band;int flags[2];
    for(unsigned order=0;order<2;order++){unsigned arm=(trial+order)%2;memset(z[arm],0,sizeof z[arm]);memset(owner[arm],0,sizeof owner[arm]);feclearexcept(FE_ALL_EXCEPT);uint64_t start=stamp();
     bool ok=arm?ksn_mesh_q7_render(q,image[arm]+y*240,y,n,z[arm],owner[arm],rows,&stats[arm]):old_q7_render(q,image[arm]+y*240,y,n,z[arm],owner[arm],rows,&stats[arm]);
     ns[arm]+=stamp()-start;flags[arm]=fetestexcept(FE_ALL_EXCEPT);assert(ok);
    }
    assert(flags[0]==flags[1]&&!memcmp(z[0],z[1],sizeof z[0])&&!memcmp(owner[0],owner[1],sizeof owner[0]));
   }
   assert(!memcmp(image[0],image[1],sizeof image[0]));for(unsigned i=0;i<240*135;i++)hash=(hash^image[0][i])*16777619u;
  }
  assert(!memcmp(&stats[0],&stats[1],sizeof stats[0]));assert(hash==0x924db725u);
  printf("ROW_REUSE trial=%u band=%u rows=%u baseline_ns=%llu candidate_ns=%llu digest=%08x depth_fast=%llu depth_exact=%llu mask_fast=%llu mask_exact=%llu spans=%llu rejects=%llu masks=%llu snapshot=%zu\n",trial,band,rows,(unsigned long long)ns[0],(unsigned long long)ns[1],hash,(unsigned long long)stats[0].depth_fast,(unsigned long long)stats[0].depth_exact,(unsigned long long)stats[0].mask_fast,(unsigned long long)stats[0].mask_exact,(unsigned long long)stats[0].span_pixels,(unsigned long long)stats[0].depth_reject,(unsigned long long)stats[0].mask_evals,qb);
 }
 free(p);free(q);
}
