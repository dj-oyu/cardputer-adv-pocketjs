#include "ui/kasane/ksn_mesh_prepared.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static unsigned rng=7;
static unsigned rnd(void){rng=rng*1664525u+1013904223u;return rng;}
static void fixture(ksn_mesh_snapshot *s,unsigned n,unsigned mode){
 s->packets.size=s->packets.packets=0;
 for(unsigned i=0;i<n;i++){
  float x=(int)(rnd()%270)-20,y=(int)(rnd()%160)-15;
  float w=1+(rnd()%48),h=1+(rnd()%30);
  if(mode==1){x=(float)(i%4)*40;y=20;w=50;h=80;}
  if(mode==2){x=-4096;y=-4096;w=1;h=1;}
  co_vertex v[3]={{x,y,-1,0,0xf800,0x07e0},{x+w,y,2,0,0x001f,0xffe0},{x,y+h,-1,0,0xffff,0}};
  for(unsigned j=0;j<3;j++)v[j].depth=mode==1?1.0f:(float)((int)(rnd()%200)-100)/13;
  uint16_t ix[6]={0,1,2,0,1,2},back[6]={31,65535,2016,0,63488,65504};uint8_t bits[8]={2};
  co_sided_input in={{v,ix,(uint16_t)i,3,2},back,bits};
  assert(co_append_sided(&s->packets,&in));
 }
}
static void compare(const ksn_mesh_snapshot *s,const ksn_mesh_prepared *p){
 uint16_t a[240*8],b[240*8];float za[240*8],zb[240*8];
 for(unsigned rows=1;rows<=8;rows*=8)for(unsigned y=0;y<135;y+=rows){
  unsigned n=rows;if(n>135-y)n=135-y;
  for(unsigned i=0;i<240*8;i++)a[i]=b[i]=0x1234;
  assert(ksn_mesh_render(s,a,y,n,za,rows));
  assert(ksn_mesh_prepared_render(p,b,y,n,zb,rows));
  assert(!memcmp(a,b,sizeof a)&&!memcmp(za,zb,240*n*sizeof(float)));
 }
}
int main(void){
 uint8_t data[65536];ksn_mesh_snapshot s={.packets={data,sizeof data,0,0},.enabled=true};
 size_t max=sizeof(ksn_mesh_prepared)+64*sizeof(ksn_prepared_triangle);
 ksn_mesh_prepared *p=calloc(1,max),*committed=calloc(1,max);assert(p&&committed);
 for(unsigned scene=0;scene<150;scene++){
  fixture(&s,1+rnd()%32,scene%3);size_t n=ksn_mesh_prepared_bytes(&s);assert(n&&n<=max);
  assert(ksn_mesh_prepare(&s,p,n));compare(&s,p);
 }
 /* Empty scope/strip and screen boundary contract, with sentinel storage. */
 s.packets.size=s.packets.packets=0;assert(ksn_mesh_prepare(&s,p,max));compare(&s,p);
 uint16_t sentinel=0xbeef;float untouched=123;
 assert(ksn_mesh_prepared_render(p,&sentinel,135,0,&untouched,1));
 assert(sentinel==0xbeef&&untouched==123);
 assert(!ksn_mesh_prepared_render(p,&sentinel,136,0,&untouched,1));
 assert(!ksn_mesh_prepared_render(p,&sentinel,135,1,&untouched,1));
 co_vertex boundary[3]={{-0.5f,-0.5f,-1,0,31,65535},{239.5f,-0.5f,1,0,2016,0},{-0.5f,134.5f,-1,0,63488,31}};
 uint16_t bix[3]={0,1,2};co_input bin={boundary,bix,0,3,1};assert(co_append(&s.packets,&bin));
 assert(ksn_mesh_prepare(&s,p,max));compare(&s,p);
 fixture(&s,4,1);size_t n=ksn_mesh_prepared_bytes(&s);assert(ksn_mesh_prepare(&s,committed,n));
 unsigned char *saved=malloc(n);assert(saved);memcpy(saved,committed,n);
 assert(!ksn_mesh_prepare(&s,p,sizeof *p));assert(!p->valid);assert(!memcmp(saved,committed,n));
 assert(ksn_mesh_prepare(&s,p,n));compare(&s,p);
 /* Prepared owns all vertices/side colors: mutation of source cannot change it. */
 memset(data,0xa5,s.packets.size);assert(!memcmp(saved,committed,n));
 uint16_t a[240],b[240];float za[240],zb[240];
 for(unsigned y=0;y<135;y++){memset(a,0,sizeof a);memset(b,0,sizeof b);assert(ksn_mesh_prepared_render(p,a,y,1,za,1));assert(ksn_mesh_prepared_render(committed,b,y,1,zb,1));assert(!memcmp(a,b,sizeof a)&&!memcmp(za,zb,sizeof za));}
 assert(!ksn_mesh_prepare(&s,p,n)&&!p->valid);assert(!memcmp(saved,committed,n));
 fixture(&s,32,0);assert(ksn_mesh_prepared_bytes(&s)==max);assert(ksn_mesh_prepare(&s,p,max));
 fixture(&s,33,0);assert(!ksn_mesh_prepared_bytes(&s));
 fixture(&s,4,1);assert(ksn_mesh_prepare(&s,p,n));
 clock_t start=clock();for(unsigned i=0;i<20;i++)for(unsigned y=0;y<135;y++)assert(ksn_mesh_render(&s,a,y,1,za,1));
 double old=1e6*(clock()-start)/CLOCKS_PER_SEC/20;
 start=clock();for(unsigned i=0;i<20;i++){assert(ksn_mesh_prepared_bytes(&s)==n);assert(ksn_mesh_prepare(&s,p,n));for(unsigned y=0;y<135;y++)assert(ksn_mesh_prepared_render(p,b,y,1,zb,1));}
 double prepared=1e6*(clock()-start)/CLOCKS_PER_SEC/20;
 printf("PREPARED_PASS scenes=150 RGB_depth_byte_equal rows=1,8 source_owned invalid_candidate_committed_unchanged limit64 empty_boundary_strips header=%zu triangle=%zu snapshot8=%zu pair8=%zu snapshot64=%zu pair64=%zu host_us=%.1f->%.1f (includes_size_and_prepare;not_target)\n",sizeof *p,sizeof p->triangles[0],n,2*n,max,2*max,old,prepared);
#ifdef KSN_MESH_EDGE4_HOST_STATS
 printf("EDGE4_RENDER_STATS blocks=%llu empty=%llu bbox_pixels=%llu safe_rejects=%llu RGB_depth_byte_equal=1\n",(unsigned long long)ksn_mesh_prepared_edge4_stats[0],(unsigned long long)ksn_mesh_prepared_edge4_stats[1],(unsigned long long)ksn_mesh_prepared_edge4_stats[2],(unsigned long long)ksn_mesh_prepared_edge4_stats[3]);
#endif
 free(saved);free(p);free(committed);return 0;
}
