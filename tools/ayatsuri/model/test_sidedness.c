#include "ayatsuri/ayatsuri_model.h"
#include "ayatsuri/ayatsuri_shade.h"
#include "ui/kasane/ksn_shiki_frame.h"
#include "ui/kasane/ksn_mesh_depth.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static uint32_t u32(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static unsigned u16(const uint8_t *p){return p[0]|(unsigned)p[1]<<8;}
static unsigned section(uint8_t *b,unsigned id){for(unsigned j=0;j<u32(b+12);j++)if(u16(b+32+j*20)==id)return 32+j*20;return 0;}
static uint8_t *load(const char *name,size_t *size){char path[256];snprintf(path,sizeof path,".cache/ayatsuri-native/sided-%s.aynm",name);FILE *f=fopen(path,"rb");assert(f);fseek(f,0,SEEK_END);*size=(size_t)ftell(f);rewind(f);uint8_t *b=malloc(*size);assert(b&&fread(b,1,*size,f)==*size);fclose(f);return b;}
static void reject(uint8_t *b,size_t size){ayatsuri_model m,sentinel;memset(&m,0x5a,sizeof m);sentinel=m;assert(ayatsuri_model_open(b,size,&m)==AYATSURI_MODEL_INVALID&&!memcmp(&m,&sentinel,sizeof m));}
int main(void){
 const char *names[]={"legacy64","sided64","backfirst","back","front","single32","sided32"};size_t sizes[7],arenas[7];
 for(unsigned test=0;test<7;test++){
  uint8_t *bytes=load(names[test],sizes+test);ayatsuri_model m;assert(ayatsuri_model_open(bytes,sizes[test],&m)==AYATSURI_MODEL_OK);
  arenas[test]=ayatsuri_model_instance_bytes(&m);void *arena=malloc(arenas[test]);assert(arena);unsigned owner=1;ayatsuri_model_instance i;
  assert(ayatsuri_model_instance_init(&m,arena,arenas[test],&owner,1,&i)==AYATSURI_MODEL_OK);
  float camera[9]={0,0,400,0,0,0,1,0,0},light[6]={0,0,1,1,1,1};
  assert(ayatsuri_model_instance_evaluate(&i,&owner,1,camera,light)==AYATSURI_MODEL_OK);
  uint32_t cursor=0;ayatsuri_model_packet p;assert(ayatsuri_model_instance_next(&i,&owner,1,&cursor,&p)==AYATSURI_MODEL_OK);
  if(test==0||test==5){assert(p.kind==4);ayatsuri_model_mesh_view v;assert(ayatsuri_model_instance_mesh_view(&i,&owner,1,1,&v)==AYATSURI_MODEL_OK);}
  else{
   assert(p.kind==5);ayatsuri_model_sided_mesh_view v,sentinel;memset(&sentinel,0xa5,sizeof sentinel);v=sentinel;
   assert(ayatsuri_model_instance_sided_mesh_view(&i,&cursor,1,1,&v)==AYATSURI_MODEL_STATE&&!memcmp(&v,&sentinel,sizeof v));
   assert(ayatsuri_model_instance_sided_mesh_view(&i,&owner,1,1,&v)==AYATSURI_MODEL_OK);
   ayatsuri_model_mesh_view old;assert(ayatsuri_model_instance_mesh_view(&i,&owner,1,1,&old)==AYATSURI_MODEL_INVALID);
   const ksn_proc_gouraud_patch *q=v.patch;
   assert(q->header.vertices==(test==6?32:3));assert(q->header.triangles==(test==6?30:(test==3||test==4)?1:64));
   for(unsigned t=0;t<q->header.triangles;t++){
    assert(((v.back_faces[t/8]>>(t%8))&1u)==((t+(test==2||test==3))%2));
    const ksn_proc_gouraud_vertex *a=q->vertices+q->indices[3*t],*b=q->vertices+q->indices[3*t+1],*c=q->vertices+q->indices[3*t+2];
    assert(((double)b->x-a->x)*((double)c->y-a->y)-((double)b->y-a->y)*((double)c->x-a->x)>1e-5);
   }
   assert(q->vertices[0].color_a==0xffff&&v.back_colors[0]==0);
   // Flip world X only: logical face side and lighting stay unchanged, screen
   // winding is normalized separately. Static bake is tested in importer.
   uint8_t dirty[2]={0,AYATSURI_TARGET_SCALE};float pose[18]={0};uint32_t color[2]={0};double opacity[2]={1,1};
   assert(ayatsuri_model_instance_read_pose(&i,&owner,1,1,pose+9)==AYATSURI_MODEL_OK);pose[15]=-1;
   ayatsuri_model_targets target={dirty,pose,color,opacity,2};assert(ayatsuri_model_instance_targets(&i,&owner,1,&target)==AYATSURI_MODEL_OK);
   assert(ayatsuri_model_instance_sided_mesh_view(&i,&owner,1,1,&v)==AYATSURI_MODEL_STATE);
   assert(ayatsuri_model_instance_evaluate(&i,&owner,1,camera,light)==AYATSURI_MODEL_OK);
   assert(ayatsuri_model_instance_sided_mesh_view(&i,&owner,1,1,&v)==AYATSURI_MODEL_OK);
   assert((v.back_faces[0]&1u)==(test==2||test==3));assert(((const ksn_proc_gouraud_patch *)v.patch)->vertices[0].color_a==0xffff&&v.back_colors[0]==0);
  }
  clock_t start=clock();for(unsigned j=0;j<100;j++){camera[7]=(float)(j&1);assert(ayatsuri_model_instance_evaluate(&i,&owner,1,camera,light)==AYATSURI_MODEL_OK);}
  printf("SIDED_CORE name=%s cold=%zu arena=%zu eval100_us=%.0f\n",names[test],sizes[test],arenas[test],1e6*(clock()-start)/CLOCKS_PER_SEC);
  assert(ayatsuri_model_instance_dispose(&i,&owner,1)==AYATSURI_MODEL_OK);free(arena);
  if(test==1){
   unsigned dir=section(bytes,13);assert(dir);unsigned at=u32(bytes+dir+4);uint8_t *copy=malloc(sizes[test]);assert(copy);
   for(unsigned offset=0;offset<4;offset++){memcpy(copy,bytes,sizes[test]);copy[at+offset]=2;reject(copy,sizes[test]);}
   memcpy(copy,bytes,sizes[test]);copy[dir]=14;reject(copy,sizes[test]);
   memcpy(copy,bytes,sizes[test]);copy[dir+2]=0;reject(copy,sizes[test]);
   memcpy(copy,bytes,sizes[test]);copy[dir+12]=0;reject(copy,sizes[test]);
   memcpy(copy,bytes,sizes[test]);copy[u32(copy+section(copy,9)+4)+19]=1;reject(copy,sizes[test]);
   for(size_t cut=0;cut<sizes[test];cut++)reject(bytes,cut);
   free(copy);
  }
  free(bytes);
 }
 for(unsigned test=0;test<2;test++){
  size_t size;uint8_t *b=load(test?"angled":"sorted",&size);ayatsuri_model m;assert(ayatsuri_model_open(b,size,&m)==AYATSURI_MODEL_OK);
  size_t length=ayatsuri_model_instance_bytes(&m);void *arena=malloc(length);assert(arena);ayatsuri_model_instance i;unsigned owner=1;
  assert(ayatsuri_model_instance_init(&m,arena,length,&owner,1,&i)==AYATSURI_MODEL_OK);
  float camera[9]={0,0,400,0,0,0,1,0,0},light[6]={.3f,-.4f,.8660254f,1,.7f,.4f};
  assert(ayatsuri_model_instance_evaluate(&i,&owner,1,camera,light)==AYATSURI_MODEL_OK);
  ayatsuri_model_sided_mesh_view view;assert(ayatsuri_model_instance_sided_mesh_view(&i,&owner,1,1,&view)==AYATSURI_MODEL_OK);
  const ksn_proc_gouraud_patch *q=view.patch;
  assert((view.back_faces[0]&1u)==1u);
  if(!test)assert(q->header.triangles==2&&q->indices[0]==3&&q->indices[3]==0&&view.back_faces[0]==1);
  else{
   // Independent scalar inverse-scale and signed quantization from the actual
   // packed F32 normal. Same public shader, no packet-color oracle shortcut.
   const uint8_t *cold=b+m.section[9].offset;double n[3],len;
   const float scales[3]={1.3f,.7f,2.2f};
   for(unsigned j=0;j<3;j++){uint32_t bits=u32(cold+12+4*j);float value;memcpy(&value,&bits,4);n[j]=(double)value/scales[j];}
   len=hypot(hypot(n[0],n[1]),n[2]);int16_t sn[3];for(unsigned j=0;j<3;j++)sn[j]=(int16_t)floor(-n[j]*32767/len+.5);
   float material[8]={.1f,.6f,.02f,8,.2f,.8f,.5f,.3f};ayatsuri_shade_basis basis;assert(ayatsuri_shade_prepare(i.lighting,&basis));
   assert(view.back_colors[0]==ayatsuri_shade_sample(sn,0xf800,175,i.lighting,material,&basis));
   assert(view.back_colors[1]==ayatsuri_shade_sample(sn,0x07e0,175,i.lighting,material,&basis));
  }
  unsigned cursor=0;ksn_proc_gouraud_patch part,sentinel;memset(&sentinel,0xa5,sizeof sentinel);part=sentinel;
  assert(!ksn_mesh_sided_run(q,view.back_colors,view.back_faces,&cursor,(void *)q)&&cursor==0);
  assert(ksn_mesh_sided_run(q,view.back_colors,view.back_faces,&cursor,&part));assert(cursor==1&&part.header.triangles==1&&part.vertices[0].color_a==view.back_colors[0]);
  if(!test){assert(ksn_mesh_sided_run(q,view.back_colors,view.back_faces,&cursor,&part)&&cursor==2&&part.vertices[0].color_a==q->vertices[0].color_a);}
  part=sentinel;assert(!ksn_mesh_sided_run(q,view.back_colors,view.back_faces,&cursor,&part)&&!memcmp(&part,&sentinel,sizeof part));
  ayatsuri_model_instance_dispose(&i,&owner,1);free(arena);free(b);
 }
 puts("SIDED_NORMAL_SORT_RUN_PASS prequant_inverse_scale AB_lighting global_centroid_order positive_run overlap_atomic");
 assert(arenas[1]-arenas[0]==24);assert(arenas[6]-arenas[5]==136);
 puts("SIDED_CORE_PASS required_section reserved_zero mirrored_state winding side_normals old_view_refusal arena_bounds");
}
