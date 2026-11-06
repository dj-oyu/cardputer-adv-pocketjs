#include "ui/kasane/ksn_mesh_q7_certified.h"
#include <math.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>
#include <fenv.h>
#if defined(__SSE__)
#include <xmmintrin.h>
#endif
extern bool baseline_ksn_mesh_q7_render(const ksn_mesh_q7_snapshot*,uint16_t*,unsigned,unsigned,float*,uint8_t*,unsigned,ksn_mesh_q7_stats*);
static unsigned rng=17;static unsigned rnd(void){return rng=rng*1664525+1013904223;}
static float bits(uint32_t u){float f;memcpy(&f,&u,4);return f;}
static void compare(ksn_mesh_q7_snapshot*q,ksn_mesh_q7_stats*stats){
 uint16_t a[1920],b[1920];float za[1920],zb[1920];uint8_t oa[1920],ob[1920];ksn_mesh_q7_stats base={0};
 for(unsigned rows=1;rows<=8;rows*=8)for(unsigned y=0;y<135;y+=rows){unsigned n=135-y;if(n>rows)n=rows;memset(a,0x35,sizeof a);memset(b,0x35,sizeof b);memset(za,0,sizeof za);memset(zb,0,sizeof zb);memset(oa,255,sizeof oa);memset(ob,255,sizeof ob);
  bool ar=ksn_mesh_q7_render(q,a,y,n,za,oa,rows,stats),br=baseline_ksn_mesh_q7_render(q,b,y,n,zb,ob,rows,&base);assert(ar==br);assert(!memcmp(a,b,n*240*2));assert(!memcmp(za,zb,n*240*4));assert(!memcmp(oa,ob,n*240));
 }
}
int main(void){
 uint8_t packets[65536];ksn_mesh_snapshot s={.packets={packets,sizeof packets,0,0},.enabled=true};size_t pb=sizeof(ksn_mesh_prepared)+64*sizeof(ksn_prepared_triangle),qb=sizeof(ksn_mesh_q7_snapshot)+64*sizeof(ksn_mesh_q7_triangle);ksn_mesh_prepared*p=malloc(pb);ksn_mesh_q7_snapshot*q=malloc(qb);assert(p&&q);ksn_mesh_q7_stats stats={0};
 const float special[]={0.f,-0.f,FLT_TRUE_MIN,-FLT_TRUE_MIN,FLT_MIN,-FLT_MIN,FLT_MAX,-FLT_MAX,1.f,-1.f,.5f,0x1.000002p0f,0x1.fffffep-1f,1e-30f,1e30f};
 for(unsigned scene=0;scene<600;scene++){s.packets.size=s.packets.packets=0;
  for(unsigned face=0;face<2;face++){co_vertex v[3];float x=scene%5?10.001f:0,y=scene%5?10.002f:0,w=scene%5?120.1f:2,h=scene%5?90.2f:2;
   for(unsigned k=0;k<3;k++){float d=special[(scene+k)%15],m=special[(scene*3+k)%15];if(scene>=200){d=bits(rnd()&0xfeffffffu);m=bits(rnd()&0xfeffffffu);}if(face&&isfinite(d))d=nextafterf(d,0);
    v[k]=(co_vertex){x+(k==1?w:0),y+(k==2?h:0),m,d,63488,2016};}
   if(scene>=400){for(unsigned k=0;k<3;k++){v[k].x=((int)(rnd()%44000)-10000)/100.f;v[k].y=((int)(rnd()%32000)-10000)/100.f;}
    double area=((double)v[1].x-v[0].x)*((double)v[2].y-v[0].y)-((double)v[1].y-v[0].y)*((double)v[2].x-v[0].x);if(area<0){co_vertex z=v[1];v[1]=v[2];v[2]=z;}}
   if(scene%11==0){v[0].depth=1;v[1].depth=nextafterf(1,2);v[2].depth=1;}
   if(scene%13==0){v[0].mask=-1;v[1].mask=1;v[2].mask=0;}
   uint16_t ix[3]={0,1,2};co_input in={v,ix,0,3,1};assert(co_append(&s.packets,&in));
  }
  assert(ksn_mesh_prepare(&s,p,pb)&&ksn_mesh_q7_prepare(p,q,qb));compare(q,&stats);
 }
 // Non-nearest rounding disables certification and preserves the baseline path.
 assert(fesetround(FE_DOWNWARD)==0);ksn_mesh_q7_stats before=stats;compare(q,&stats);assert(stats.depth_fast==before.depth_fast&&stats.mask_fast==before.mask_fast);assert(fesetround(FE_TONEAREST)==0);
#if defined(__SSE__)
 // Certificate preparation under DAZ must not survive after DAZ is cleared.
 s.packets.size=s.packets.packets=0;co_vertex transition[3]={{0,0,-nextafterf(FLT_MIN,0),1,63488,2016},{4,0,FLT_MIN,1,63488,2016},{0,4,0,1,63488,2016}};uint16_t ti[3]={0,1,2};co_input tin={transition,ti,0,3,1};assert(co_append(&s.packets,&tin));assert(ksn_mesh_prepare(&s,p,pb));
 unsigned csr=_mm_getcsr();_mm_setcsr(csr|0x8040u);assert(ksn_mesh_q7_prepare(p,q,qb));_mm_setcsr(csr);assert(!q->triangles[0].cert.valid);before=stats;compare(q,&stats);assert(stats.depth_fast==before.depth_fast&&stats.mask_fast==before.mask_fast);
 assert(ksn_mesh_q7_prepare(p,q,qb));compare(q,&stats);
 csr=_mm_getcsr();_mm_setcsr(csr|0x8040u);before=stats;compare(q,&stats);assert(stats.depth_fast==before.depth_fast&&stats.mask_fast==before.mask_fast);_mm_setcsr(csr);
#endif
 printf("CERT_PASS scenes=600 rows1and8 partial_failure_bytes_equal rounding_gate depth_fast=%llu depth_exact=%llu mask_fast=%llu mask_exact=%llu\n",(unsigned long long)stats.depth_fast,(unsigned long long)stats.depth_exact,(unsigned long long)stats.mask_fast,(unsigned long long)stats.mask_exact);
 free(p);free(q);return 0;
}
