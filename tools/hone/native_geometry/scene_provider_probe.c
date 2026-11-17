/* Host-only diagnostic: source rest geometry with placeholder materials.
 * Not a runtime-ready model, visual correctness check, firmware or FPS test. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "geometry.h"
#include "native_gather.h"
#include "ayatsuri/ayatsuri_model_provider.h"
#include "native_provider.h"
#include "provider_nodes.h"
#ifdef HONE_PROBE_NODE_DRIVER
#include "driver_programs.h"
#include "driver_nodes.h"
#include "scene_pose.h"
#endif
#ifdef HONE_PROBE_MORPH_DRIVER
#include "native_morph.h"
#include "native_dq.h"
#include "native_deform.h"
#include "morph_targets.h"
#endif
int polygon_normals(const float*,size_t,const uint32_t*,const uint32_t*,const uint8_t*,size_t,size_t,double*,float*);
typedef struct {hone_provider_context context;unsigned calls;} probe_context;
static bool provider(void *p,unsigned id,unsigned count,ayatsuri_skin_result *slot){
 probe_context *ctx=p;ctx->calls++;return hone_native_provider(&ctx->context,id,count,slot);
}
int main(int argc,char **argv){
 assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f);assert(!fseek(f,0,SEEK_END));long bytes=ftell(f);assert(bytes>0);rewind(f);
 void *blob=malloc((size_t)bytes);assert(blob&&fread(blob,1,(size_t)bytes,f)==(size_t)bytes);fclose(f);
 ayatsuri_model strict,m;ayatsuri_model_status strict_status=ayatsuri_model_open(blob,(size_t)bytes,&strict);
 assert(ayatsuri_model_open_provider(blob,(size_t)bytes,&m)==AYATSURI_MODEL_OK);
 assert(m.node_count==hone_provider_node_count&&hone_input_mesh_count);
 hone_provider_mesh *meshes=calloc(hone_input_mesh_count,sizeof *meshes);assert(meshes);
 size_t max_vertices=0,expected_calls=0;
 for(size_t i=0;i<hone_input_mesh_count;i++)if(hone_input_meshes[i].source_vertex_count>max_vertices)max_vertices=hone_input_meshes[i].source_vertex_count;
 for(size_t i=0;i<hone_provider_node_count;i++)if(hone_provider_nodes[i].count)expected_calls++;
 double *work=malloc(max_vertices*3*sizeof *work);assert(work);
#ifdef HONE_PROBE_MORPH_DRIVER
 float **positions=calloc(hone_input_mesh_count,sizeof *positions),**masks=calloc(hone_input_mesh_count,sizeof *masks);
 assert(positions&&masks&&sizeof hone_morph_offsets/sizeof hone_morph_offsets[0]==hone_input_mesh_count+1);
#endif
 for(size_t i=0;i<hone_input_mesh_count;i++){
  const hone_input_mesh *x=&hone_input_meshes[i];float *normals=malloc((size_t)x->polygon_corner_count*3*sizeof *normals);assert(normals);
  assert(polygon_normals(x->source_position,x->source_vertex_count,x->polygon_offsets,x->polygon_vertices,x->polygon_smooth,x->polygon_count,x->polygon_corner_count,work,normals));
  meshes[i]=(hone_provider_mesh){x,x->source_position,normals};
#ifdef HONE_PROBE_MORPH_DRIVER
  assert(hone_morph_offsets[i+1]-hone_morph_offsets[i]==x->morphs);
  positions[i]=malloc((size_t)x->source_vertex_count*3*sizeof(float));assert(positions[i]);
  if(x->morphs){size_t count=(size_t)x->source_vertex_count*x->morphs;masks[i]=malloc(count*sizeof(float));assert(masks[i]);for(size_t k=0;k<count;k++)masks[i][k]=1;}
#endif
 }
 probe_context ctx={{meshes,hone_input_mesh_count,hone_provider_nodes,hone_provider_node_count},0};
 size_t arena_bytes=ayatsuri_model_instance_bytes(&m);void *arena=malloc(arena_bytes);assert(arena);int owner=1;
 ayatsuri_model_instance inst;assert(ayatsuri_model_instance_init(&m,arena,arena_bytes,&owner,1,&inst)==AYATSURI_MODEL_OK);
 const float camera[9]={0,0,400,0,0,0,1,120,67},light[6]={0,0,1,1,1,1};ayatsuri_skin_result slot[32];
 assert(ayatsuri_model_instance_evaluate(&inst,&owner,1,camera,light)==AYATSURI_MODEL_INVALID);
 ayatsuri_model_status status=ayatsuri_model_candidate_evaluate_provider(&inst,&owner,1,camera,light,provider,&ctx,slot);
 printf("strict_open=%d probe_status=%d nodes=%u provider_calls=%u model_bytes=%ld instance_arena_bytes=%zu materials=placeholder runtime_ready=false\n",strict_status,status,m.node_count,ctx.calls,bytes,arena_bytes);
 assert(status==AYATSURI_MODEL_OK&&ctx.calls==expected_calls);
#ifdef HONE_PROBE_NODE_DRIVER
 assert(hone_probe_node_count==m.node_count&&hone_driver_input_count&&hone_driver_program_count);
 double *inputs=calloc(hone_driver_input_count,sizeof *inputs),*scratch=calloc(hone_driver_program_count,sizeof *scratch),*values=calloc(hone_driver_program_count,sizeof *values),*opacity=calloc(m.node_count,sizeof *opacity);
 float *pose=calloc((size_t)m.node_count*9,sizeof *pose);uint8_t *dirty=calloc(m.node_count,1);uint32_t *colors=calloc(m.node_count,sizeof *colors);
 assert(inputs&&scratch&&values&&opacity&&pose&&dirty&&colors);
#ifdef HONE_PROBE_MORPH_DRIVER
 float *morphs=malloc(hone_morph_count*sizeof *morphs);assert(morphs);
#endif
 for(unsigned frame=0;frame<4;frame++){
  const double sample[4]={0,.25,.75,1};for(size_t k=0;k<hone_driver_input_count;k++)inputs[k]=sample[frame];
  assert(hone_driver_frame(hone_driver_programs,hone_driver_program_count,hone_driver_input_slots,hone_driver_slot_count,inputs,hone_driver_input_count,scratch,values));
#ifdef HONE_PROBE_MORPH_DRIVER
  assert(hone_driver_morph_targets(hone_morph_routes,hone_morph_route_count,values,hone_driver_program_count,hone_morph_defaults,hone_morph_count,morphs));
  for(size_t k=0;k<hone_input_mesh_count;k++){
   const hone_input_mesh *msh=&hone_input_meshes[k];
   assert(hone_deform_source(msh,msh->morphs?morphs+hone_morph_offsets[k]:NULL,masks[k],msh->morphs,0,NULL,0,NULL,0,NULL,NULL,positions[k]));
   assert(polygon_normals(positions[k],msh->source_vertex_count,msh->polygon_offsets,msh->polygon_vertices,msh->polygon_smooth,msh->polygon_count,msh->polygon_corner_count,work,(float*)meshes[k].packed_normals));
   meshes[k].source_positions=positions[k];
  }
#endif
  assert(hone_driver_node_targets(hone_driver_node_routes,hone_driver_node_route_count,values,hone_driver_program_count,hone_probe_base_pose,m.node_count,dirty,pose));
  ayatsuri_model_targets targets={dirty,pose,colors,opacity,m.node_count};
  assert(ayatsuri_model_instance_targets(&inst,&owner,1,&targets)==AYATSURI_MODEL_OK);
  unsigned before=ctx.calls;
  assert(ayatsuri_model_candidate_evaluate_provider(&inst,&owner,1,camera,light,provider,&ctx,slot)==AYATSURI_MODEL_OK);
  assert(ctx.calls-before==expected_calls);
 }
#ifdef HONE_PROBE_MORPH_DRIVER
 printf("PASS node and unmasked morph driver frames=4 provider_calls=%u rig_shrinkwrap_material=unapplied runtime_ready=false\n",ctx.calls);
 free(morphs);
#else
 printf("PASS node driver frames=4 provider_calls=%u rig_morph_material_outputs=unapplied runtime_ready=false\n",ctx.calls);
#endif
 free(inputs);free(scratch);free(values);free(opacity);free(pose);free(dirty);free(colors);
#endif
 assert(ayatsuri_model_instance_dispose(&inst,&owner,1)==AYATSURI_MODEL_OK);
 for(size_t i=0;i<hone_input_mesh_count;i++)free((void*)meshes[i].packed_normals);
#ifdef HONE_PROBE_MORPH_DRIVER
 for(size_t i=0;i<hone_input_mesh_count;i++){free(positions[i]);free(masks[i]);}free(positions);free(masks);
#endif
 free(work);free(meshes);free(arena);free(blob);return 0;
}
