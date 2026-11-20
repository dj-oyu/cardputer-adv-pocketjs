#include <assert.h>
#include <stdio.h>
#include "alpha_bounds_frame_candidate.h"
static uint16_t expected[240*135],actual[240*135];
static float z[240];
static void parity(ksn_mesh_snapshot*opaque,hone_alpha_packet*p,size_t count,unsigned span){
 int owner=1;hone_alpha_bounds bounds[32];
 hone_alpha_snapshot a={.packets=p,.count=count,.capacity=32,.owner=&owner,.generation=1,.ready=1},b=a;
 hone_frame_lane old={.opaque=opaque,.alpha=&a,.owner=&owner,.generation=1};
 hone_bounds_frame_lane fresh={.opaque=opaque,.alpha=&b,.owner=&owner,.generation=1,.bounds=bounds,.bounds_capacity=32};
 int ok=hone_frame_preflight(&old,&owner,1,span);
 assert(ok==hone_bounds_frame_preflight(&fresh,&owner,1,span));
 if(!ok){assert(!fresh.validated);return;}
 assert(hone_frame_pin(&old,&owner,1));assert(hone_bounds_frame_pin(&fresh,&owner,1));
 assert(!hone_bounds_frame_preflight(&fresh,&owner,1,span)&&fresh.pinned&&fresh.validated);
 for(unsigned band=1;band<=8;band*=2){
  for(unsigned i=0;i<240*135;i++)expected[i]=actual[i]=0x1234;
  for(unsigned y=0;y<135;y+=band){unsigned h=135-y<band?135-y:band;
   assert(hone_frame_band(&old,&owner,1,expected+y*240,y,h,z));
   assert(hone_bounds_frame_band(&fresh,&owner,1,actual+y*240,y,h,z));
  }
  assert(!memcmp(actual,expected,sizeof actual));
 }
 uint16_t saved[240];memcpy(saved,actual,sizeof saved);
 if(count)assert(!hone_bounds_frame_band(&fresh,&owner,1,(uint16_t*)bounds,0,1,z));
 if(count)assert(!hone_bounds_frame_band(&fresh,&owner,1,actual,0,1,(float*)bounds));
 assert(!memcmp(saved,actual,sizeof saved));
 assert(hone_frame_unpin(&old,&owner,1));assert(hone_bounds_frame_unpin(&fresh,&owner,1));
}
static void lifecycle(ksn_mesh_snapshot*opaque){
 int owner=1;hone_alpha_packet packets[5],next[5];
 packets[0]=(hone_alpha_packet){.v={{0,0,2,0},{4,0,2,0},{0,4,2,0}},.rgb={1,0,0},.alpha=.5f};
 for(unsigned i=0;i<5;i++)next[i]=packets[i]=packets[0];
 hone_alpha_snapshot a={.packets=packets,.count=1,.capacity=5,.owner=&owner,.generation=1,.ready=1},replacement={.packets=next,.count=1,.capacity=5,.owner=&owner,.generation=1};
 hone_alpha_bounds bounds[5],saved_bounds[5];memset(bounds,0x55,sizeof bounds);memcpy(saved_bounds,bounds,sizeof bounds);
 hone_bounds_frame_lane c={.opaque=opaque,.alpha=&a,.owner=&owner,.generation=1,.bounds=bounds,.bounds_capacity=5},ready=c;
 /* No bounds may be written before validation of the LAST packet. */
 a.count=5;packets[4].v[2].x=NAN;
 assert(!hone_bounds_frame_preflight(&c,&owner,1,240));assert(!memcmp(bounds,saved_bounds,sizeof bounds));packets[4]=packets[0];a.count=1;
 assert(hone_bounds_frame_preflight(&c,&owner,1,240));
 assert(hone_alpha_publish(&a,&replacement,&owner,1));assert(!hone_bounds_frame_pin(&c,&owner,1));
 replacement.count=1;assert(hone_alpha_publish(&a,&replacement,&owner,1));assert(a.packets==packets&&a.revision==2);assert(!hone_bounds_frame_pin(&c,&owner,1));
 assert(hone_bounds_frame_preflight(&c,&owner,1,240));assert(hone_bounds_frame_commit(&ready,&c,&owner,1));
 hone_bounds_frame_lane saved_ready=ready;
 /* Failed candidate preflight/commit must not mutate the ready descriptor.
  * Separate pending bounds are necessary to preserve ready cache contents. */
 hone_alpha_bounds pending[5];c.bounds=pending;c.bounds_capacity=0;
 assert(!hone_bounds_frame_preflight(&c,&owner,1,240));assert(!hone_bounds_frame_commit(&ready,&c,&owner,1));assert(!memcmp(&ready,&saved_ready,sizeof ready));
 assert(hone_bounds_frame_pin(&ready,&owner,1));assert(!hone_alpha_publish(&a,&replacement,&owner,1));
 assert(hone_bounds_frame_unpin(&ready,&owner,1));
 c.bounds_capacity=5;assert(hone_bounds_frame_preflight(&c,&owner,1,240));a.count=2;assert(!hone_bounds_frame_pin(&c,&owner,1));a.count=1;
 c.bounds=(hone_alpha_bounds*)packets;assert(!hone_bounds_frame_preflight(&c,&owner,1,240));
 c.bounds=pending;assert(hone_bounds_frame_preflight(&c,&owner,1,240));assert(!hone_bounds_frame_preflight(&c,&owner,2,240));assert(hone_bounds_frame_pin(&c,&owner,1));assert(hone_bounds_frame_unpin(&c,&owner,1));
}
int main(void){
 unsigned char data[1024];ksn_mesh_snapshot opaque={.enabled=1};
 co_vertex vertices[3]={{0,0,0,1,0xffff,0xffff},{100,0,0,1,0xffff,0xffff},{0,100,0,1,0xffff,0xffff}};
 uint16_t indices[]={0,1,2};co_input input={vertices,indices,1,3,1};opaque.packets=(co_buffer){data,sizeof data,0,0};assert(co_append(&opaque.packets,&input));
 hone_alpha_packet p[32];
 for(unsigned i=0;i<32;i++){
  float x=(i%8)*30.f,y=(i/8)*30.f;
  p[i]=(hone_alpha_packet){.v={{x,y,2,0},{x+18,y,2,2},{x,y+18,2,0}},.rgb={.3f,.7f,.2f},.alpha=.37f,.mask_enabled=i%2,.mask_low=.5f,.mask_high=1.5f,.order=i%3};
 }
 parity(&opaque,p,0,240);parity(&opaque,p,32,240);parity(&opaque,p,32,4);
 for(unsigned i=0;i<5;i++)p[i]=p[0];
 parity(&opaque,p,5,240); /* conservative fifth layer rejection */
 parity(&opaque,p,4,240); /* exact depth ties and reverse order */
 p[0].alpha=0;parity(&opaque,p,4,240);
 for(unsigned i=0;i<4;i++)for(unsigned j=0;j<3;j++)p[i].v[j].z=-2;
 parity(&opaque,p,4,240); /* same opaque occlusion */
 lifecycle(&opaque);
 puts("PASS bounded frame parity bands=1,2,4,8; overflow/span/depth; late invalid; revision ABA; pin; rollback; alias");
 return 0;
}
