#include "ui/kasane/ksn_mesh_depth.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
bool legacy_co_append(co_buffer *,const co_input *);
bool legacy_co_read(const uint8_t *,size_t,co_view *);
static void codec(void){
 uint8_t a[4096]={0},b[4096]={0},saved[4096];co_vertex vertices[32],selected[32];uint16_t indices[192],back[64];uint8_t bits[8];
 for(unsigned n=3;n<=32;n+=29)for(unsigned solid=0;solid<2;solid++){
  for(unsigned j=0;j<n;j++)vertices[j]=(co_vertex){(float)j,(float)(j%5),(float)j-2,(float)j/7,(uint16_t)(solid?31:j*23),(uint16_t)(solid?31:j*79)};
  for(unsigned t=0;t<64;t++){indices[3*t]=(uint16_t)(t%n);indices[3*t+1]=(uint16_t)((t+1)%n);indices[3*t+2]=(uint16_t)((t+2)%n);}
  co_input in={vertices,indices,42,(uint8_t)n,64};co_buffer now={a,sizeof a,0,0},old={b,sizeof b,0,0};
  assert(co_append(&now,&in)&&legacy_co_append(&old,&in)&&now.size==old.size&&!memcmp(a,b,now.size));
 }
 for(unsigned j=0;j<32;j++){vertices[j]=(co_vertex){(float)j,(float)(j%5),(float)j-2,(float)j/7,(uint16_t)(j*23),(uint16_t)(j*79)};back[2*j]=(uint16_t)(65535-j);back[2*j+1]=(uint16_t)(32000-j);}
 for(unsigned t=0;t<64;t++){indices[3*t]=(uint16_t)(t%32);indices[3*t+1]=(uint16_t)((t+1)%32);indices[3*t+2]=(uint16_t)((t+2)%32);}memset(bits,0xaa,8);
 co_sided_input in={{vertices,indices,42,32,64},back,bits};co_buffer now={a,sizeof a,0,0};
 assert(co_append_sided(&now,&in)&&now.size==980&&now.packets==1);co_view view;assert(co_read(a,now.size,&view)&&view.mode==2&&!legacy_co_read(a,now.size,&view));assert(co_read(a,now.size,&view));
 for(unsigned t=0;t<64;t++){bool side=false;assert(co_triangle_side(&view,t,&side)&&side==(t%2!=0));}
 for(unsigned j=0;j<32;j++){co_vertex v;assert(co_vertex_at_side(&view,j,true,&v)&&v.color_a==back[2*j]&&v.color_b==back[2*j+1]);assert(co_vertex_at_side(&view,j,false,&v)&&v.color_a==vertices[j].color_a);}
 memcpy(saved,a,sizeof a);for(size_t cut=0;cut<now.size;cut++)assert(!co_read(a,cut,&view));
 now.capacity=now.size;assert(!co_append_sided(&now,&in)&&now.size==980&&now.packets==1&&!memcmp(a,saved,sizeof a));now.capacity=sizeof a;
 now.packets=256;assert(!co_append_sided(&now,&in)&&!memcmp(a,saved,sizeof a));now.packets=1;
 vertices[0].depth=NAN;assert(!co_append_sided(&now,&in)&&!memcmp(a,saved,sizeof a));vertices[0].depth=0;
 // All front and all back reuse the exact old encoding, without dropping a
 // selected triangle. Equal palettes (e.g. unlit) also reuse the old bytes.
 for(unsigned mode=0;mode<3;mode++){
  memset(bits,mode==1?0xff:mode==2?0xaa:0,8);memcpy(selected,vertices,sizeof vertices);
  for(unsigned j=0;j<32;j++){
   if(mode==1){selected[j].color_a=back[2*j];selected[j].color_b=back[2*j+1];}
   if(mode==2){back[2*j]=vertices[j].color_a;back[2*j+1]=vertices[j].color_b;}
  }
  now=(co_buffer){a,sizeof a,0,0};co_buffer old={b,sizeof b,0,0};co_input reference=in.front;reference.vertices=selected;
  assert(co_append_sided(&now,&in)&&legacy_co_append(&old,&reference)&&now.size==old.size&&!memcmp(a,b,now.size));
 }
 // Small mode2 checks canonical side bits, padding, indices and complete copy.
 in.front.vertex_count=3;in.front.triangle_count=2;uint16_t two[6]={0,1,2,0,1,2};in.front.indices=two;bits[0]=2;back[0]=0xf800;
 now=(co_buffer){a,sizeof a,0,0};assert(co_append_sided(&now,&in)&&now.size==92);memcpy(saved,a,sizeof a);
 size_t used=now.size;bits[0]=0x80;assert(!co_append_sided(&now,&in)&&now.size==used&&!memcmp(a,saved,sizeof a));bits[0]=2;
 a[90]|=0x80;assert(!co_read(a,used,&view));memcpy(a,saved,sizeof a);a[91]=1;assert(!co_read(a,used,&view));memcpy(a,saved,sizeof a);
 a[84]=3;assert(!co_read(a,used,&view));memcpy(a,saved,sizeof a);a[2]=3;assert(!co_read(a,used,&view));memcpy(a,saved,sizeof a);
 memset(vertices,0x5a,sizeof vertices);memset(back,0x5a,sizeof back);memset(bits,0,8);
 assert(co_read(a,used,&view)&&co_triangle_side(&view,1,(bool[1]){0}));co_vertex v;assert(co_vertex_at_side(&view,0,true,&v)&&v.color_a==0xf800);
 puts("SIDE_RECORD_CODEC_PASS legacy_bytes mode2_bounds980 owned_copy malformed_truncated side_padding quota_transaction");
}
static void render_compare(unsigned first_back,bool unlit){
 uint8_t *old_data=calloc(1,65536),*new_data=calloc(1,65536);assert(old_data&&new_data);
 ksn_mesh_snapshot old={.packets={old_data,65536,0,0},.enabled=true,.work_limit=8000000},now={.packets={new_data,65536,0,0},.enabled=true,.work_limit=8000000};
 ksn_proc_gouraud_patch p={0};p.header.vertices=3;p.header.triangles=64;
 p.vertices[0]=(ksn_proc_gouraud_vertex){0,0,-1,0xf800,0x001f};p.vertices[1]=(ksn_proc_gouraud_vertex){3,0,2,0xf800,0x001f};p.vertices[2]=(ksn_proc_gouraud_vertex){0,4,-1,0xf800,0x001f};
 for(unsigned t=0;t<64;t++){p.indices[3*t]=0;p.indices[3*t+1]=1;p.indices[3*t+2]=2;}
 uint16_t colors[6]={0x07e0,0xffe0,0x07e0,0xffe0,0x07e0,0xffe0};uint8_t bits[8];memset(bits,first_back?0x55:0xaa,8);float depth[3]={0,0,0};
 if(unlit)for(unsigned j=0;j<3;j++){p.vertices[j].color_a=p.vertices[j].color_b=65535;colors[2*j]=colors[2*j+1]=65535;}
 unsigned cursor=0;ksn_proc_gouraud_patch part;
 while(cursor<64){assert(ksn_mesh_sided_run(&p,colors,bits,&cursor,&part)&&ksn_mesh_append(&old,&part,depth,1));}
 assert(ksn_mesh_append_sided(&now,&p,depth,colors,bits,1)&&now.packets.packets==1&&old.packets.packets==64&&now.work==old.work);
 uint16_t a[240],b[240];float za[240],zb[240];clock_t start=clock();
 for(unsigned repeat=0;repeat<8;repeat++)for(unsigned y=0;y<135;y++){
  for(unsigned x=0;x<240;x++)a[x]=17;
  assert(ksn_mesh_render(&old,a,y,1,za,1));
 }
 double old_us=1e6*(clock()-start)/CLOCKS_PER_SEC/8;start=clock();
 for(unsigned repeat=0;repeat<8;repeat++)for(unsigned y=0;y<135;y++){
  for(unsigned x=0;x<240;x++)b[x]=17;
  assert(ksn_mesh_render(&now,b,y,1,zb,1));
 }
 double new_us=1e6*(clock()-start)/CLOCKS_PER_SEC/8;
 for(unsigned y=0;y<135;y++){
  for(unsigned x=0;x<240;x++)a[x]=b[x]=17;
  assert(ksn_mesh_render(&old,a,y,1,za,1)&&ksn_mesh_render(&now,b,y,1,zb,1));assert(!memcmp(a,b,sizeof a)&&!memcmp(za,zb,sizeof za));
  if(y==1)assert(b[1]==(unlit?65535:first_back?0xffe0:0x001f));
 }
 printf("SIDE_RECORD_COMPARE backFirst=%u unlit=%u records=%u->%u used=%zu->%zu arena_change=0 depth_scratch=960 raster_us=%.0f->%.0f RGB_depth_byte_equal=1\n",first_back,unlit,old.packets.packets,now.packets.packets,old.packets.size,now.packets.size,old_us,new_us);
 free(old_data);free(new_data);
}
int main(void){codec();render_compare(0,false);render_compare(1,false);render_compare(0,true);puts("SIDE_RECORD_PASS");}
