#include <assert.h>
#include <stdio.h>
#include "frame_lane_candidate.h"
static void same_generation_race(void){
 int owner=1;hone_alpha_packet initial[5]={0},replacement[5]={0};
 for(int i=0;i<5;i++)replacement[i]=(hone_alpha_packet){.v={{0,130,1,0},{4,130,1,0},{0,134,1,0}},.rgb={1,0,0},.alpha=.5f};initial[0]=replacement[0];
 hone_alpha_snapshot a={initial,1,5,&owner,1,0,1,0},b={replacement,5,5,&owner,1,0,0,0};ksn_mesh_snapshot opaque={.enabled=1};hone_frame_lane lane={&opaque,&a,&owner,1,0,0,0,NULL,0};
 assert(hone_frame_preflight(&lane,&owner,1,54));assert(hone_alpha_publish(&a,&b,&owner,1));assert(!hone_frame_pin(&lane,&owner,1));
 /* ABA packet-pointer return must still reject by monotonically advanced revision. */
 b.count=1;assert(hone_alpha_publish(&a,&b,&owner,1));assert(a.packets==initial&&a.revision==2);assert(!hone_frame_pin(&lane,&owner,1));
 assert(hone_frame_preflight_pin(&lane,&owner,1,54));assert(!hone_alpha_publish(&a,&b,&owner,1));assert(hone_frame_unpin(&lane,&owner,1));
}
int main(void){same_generation_race();int owner=1;union{float align;unsigned char data[960];} store={0};hone_alpha_packet*p=(hone_alpha_packet*)store.data;
*p=(hone_alpha_packet){.v={{0,0,1,0},{4,0,1,0},{0,4,1,0}},.rgb={1,0,0},.alpha=.5f};
hone_alpha_fragment f;assert(hone_alpha_sample(p,0,0,-INFINITY,&f)==1);assert(hone_alpha_sample(p,2,2,-INFINITY,&f)==0);
ksn_mesh_snapshot opaque={.enabled=1};hone_alpha_snapshot alpha={p,1,1,&owner,1,0,1,0};hone_frame_lane lane={&opaque,&alpha,&owner,1,0,0,0,NULL,0};assert(hone_frame_preflight(&lane,&owner,1,54));assert(hone_frame_pin(&lane,&owner,1));assert(alpha.pinned);
hone_alpha_packet old=*p;uint16_t pixels[240]={0};assert(!hone_frame_band(&lane,&owner,1,pixels,0,1,(float*)store.data));assert(!memcmp(&old,p,sizeof old));
assert(hone_frame_unpin(&lane,&owner,1)&&!alpha.pinned);assert(!hone_frame_unpin(&lane,&owner,1));
/* Actual opaque edge coverage must match transparent packet coverage. */
unsigned char buffer[1024];co_vertex vertices[3]={{0,0,0,1,0xffff,0xffff},{4,0,0,1,0xffff,0xffff},{0,4,0,1,0xffff,0xffff}};uint16_t indices[]={0,1,2};co_input in={vertices,indices,1,3,1};opaque.packets=(co_buffer){buffer,sizeof buffer,0,0};assert(co_append(&opaque.packets,&in));float z[240];
for(unsigned y=0;y<5;y++){memset(pixels,0,sizeof pixels);assert(ksn_mesh_render(&opaque,pixels,y,1,z,1));for(unsigned x=0;x<5;x++)assert((pixels[x]!=0)==hone_alpha_sample(p,x,y,-INFINITY,&f));}
puts("PASS independent alias reproduction, coupled pin, actual opaque/alpha shared-edge parity");return 0;}
