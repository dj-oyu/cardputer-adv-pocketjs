/* Synthetic host probe: actual model evaluation and owned mesh depth snapshot. */
#include "ayatsuri/ayatsuri_model.h"
#include "ui/kasane/ksn_mesh_depth.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char **argv){
 assert(argc==4);FILE *f=fopen(argv[1],"rb");assert(f);assert(!fseek(f,0,SEEK_END));long length=ftell(f);assert(length>0&&length<=512*1024);rewind(f);
 unsigned char *bytes=malloc((size_t)length);assert(bytes&&fread(bytes,1,(size_t)length,f)==(size_t)length);fclose(f);
 ayatsuri_model model;assert(ayatsuri_model_open(bytes,(size_t)length,&model)==AYATSURI_MODEL_OK);
 size_t size=ayatsuri_model_instance_bytes(&model);void *arena=malloc(size);assert(arena);unsigned owner=1;ayatsuri_model_instance instance;
 assert(ayatsuri_model_instance_init(&model,arena,size,&owner,1,&instance)==AYATSURI_MODEL_OK);
 int sign=atoi(argv[3]);assert(sign==1||sign==-1);
 float camera[9]={0,0,(float)(400*sign),0,0,0,1,120,67},light[6]={0,0,1,1,1,1};
 assert(ayatsuri_model_instance_evaluate(&instance,&owner,1,camera,light)==AYATSURI_MODEL_OK);
 uint8_t storage[65536];ksn_mesh_snapshot snapshot={.packets={storage,sizeof storage,0,0},.work_limit=8000000,.enabled=true};
 for(unsigned n=1;n<model.node_count;n++){
  ayatsuri_model_mesh_view view;assert(ayatsuri_model_instance_mesh_view(&instance,&owner,1,(uint16_t)n,&view)==AYATSURI_MODEL_OK);
  if(((const ksn_proc_gouraud_patch *)view.patch)->header.triangles)assert(ksn_mesh_append(&snapshot,view.patch,view.depth,(uint16_t)n));
 }
 /* Snapshot must not retain the source actor, arena or cold bytes. */
 assert(ayatsuri_model_instance_dispose(&instance,&owner,1)==AYATSURI_MODEL_OK);free(arena);free(bytes);
 uint16_t one[240*135]={0},eight[240*135]={0};float scratch[240*8];
 assert(ksn_mesh_render(&snapshot,one,0,135,scratch,1));assert(ksn_mesh_render(&snapshot,eight,0,135,scratch,8));assert(!memcmp(one,eight,sizeof one));
 f=fopen(argv[2],"wb");assert(f);for(unsigned i=0;i<240*135;i++){assert(fputc(one[i]&255,f)!=EOF);assert(fputc(one[i]>>8,f)!=EOF);}assert(!fclose(f));
 printf("TRANSFORM_NATIVE_RENDER_PASS arena=%zu records=%u row1_row8=equal\n",size,snapshot.packets.packets);
 return 0;
}
