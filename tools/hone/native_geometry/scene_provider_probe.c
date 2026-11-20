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
#ifdef HONE_PROBE_RIG_DRIVER
#include "rig_targets.h"
#endif
#ifdef HONE_PROBE_WRAP
#include "ayatsuri/ayatsuri_transform.h"
#include "native_wrap_candidate.h"
#include "wrap_targets.h"
#endif
int polygon_normals(const float*,size_t,const uint32_t*,const uint32_t*,const uint8_t*,size_t,size_t,double*,float*);
typedef struct {hone_provider_context context;unsigned calls;} probe_context;
static bool provider(void *p,unsigned id,unsigned count,ayatsuri_skin_result *slot){
 probe_context *ctx=p;ctx->calls++;return hone_native_provider(&ctx->context,id,count,slot);
}
int main(int argc,char **argv){
#ifdef HONE_PROBE_WRAP
 assert(argc>=2&&argc<=4);FILE *position_file=argc>=3?fopen(argv[2],"wbx"):NULL;assert(argc<3||position_file);
 FILE *normal_file=argc==4?fopen(argv[3],"wbx"):NULL;assert(argc<4||normal_file);
#else
 assert(argc==2);
#endif
 FILE *f=fopen(argv[1],"rb");assert(f);assert(!fseek(f,0,SEEK_END));long bytes=ftell(f);assert(bytes>0);rewind(f);
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
#ifdef HONE_PROBE_RIG_DRIVER
 hone_root_bone *bone_scratch=malloc(hone_root_bone_count*sizeof *bone_scratch);
 double *palette_scratch=malloc(hone_root_bone_count*7*sizeof *palette_scratch),*palette=malloc(hone_root_bone_count*7*sizeof *palette);
 assert(bone_scratch&&palette_scratch&&palette);
#endif
#ifdef HONE_PROBE_WRAP
 ayatsuri_transform *worlds=calloc(m.node_count,sizeof *worlds);assert(worlds);
#endif
 for(unsigned frame=0;frame<4;frame++){
  const double sample[4]={0,.25,.75,1};for(size_t k=0;k<hone_driver_input_count;k++)inputs[k]=sample[frame];
  assert(hone_driver_frame(hone_driver_programs,hone_driver_program_count,hone_driver_input_slots,hone_driver_slot_count,inputs,hone_driver_input_count,scratch,values));
#ifdef HONE_PROBE_MORPH_DRIVER
  assert(hone_driver_morph_targets(hone_morph_routes,hone_morph_route_count,values,hone_driver_program_count,hone_morph_defaults,hone_morph_count,morphs));
#ifdef HONE_PROBE_RIG_DRIVER
  assert(hone_root_driver_palette(hone_root_bones,hone_root_bone_count,hone_root_routes,hone_root_route_count,values,hone_driver_program_count,bone_scratch,palette_scratch,palette));
#endif
  for(size_t k=0;k<hone_input_mesh_count;k++){
   const hone_input_mesh *msh=&hone_input_meshes[k];
   int skin=0;const uint32_t *groups=NULL;size_t group_count=0,joint_count=0;const double *joints=NULL,*to_rig=NULL,*from_rig=NULL;
#ifdef HONE_PROBE_RIG_DRIVER
   for(size_t j=0;j<hone_root_mesh_count;j++)if(hone_root_meshes[j].source_mesh==k){
    assert(!skin);const hone_root_mesh_binding *binding=&hone_root_meshes[j];skin=1;groups=binding->groups;group_count=binding->group_count;
    joints=palette;joint_count=hone_root_bone_count;to_rig=binding->to_rig;from_rig=binding->from_rig;
   }
#endif
   assert(hone_deform_source(msh,msh->morphs?morphs+hone_morph_offsets[k]:NULL,masks[k],msh->morphs,skin,groups,group_count,joints,joint_count,to_rig,from_rig,positions[k]));
  }
#endif
  assert(hone_driver_node_targets(hone_driver_node_routes,hone_driver_node_route_count,values,hone_driver_program_count,hone_probe_base_pose,m.node_count,dirty,pose));
#ifdef HONE_PROBE_WRAP
  for(size_t k=0;k<m.node_count;k++){
   ayatsuri_pose local;memcpy(&local,pose+k*9,sizeof local);int parent=hone_probe_parents[k];
   assert(ayatsuri_transform_evaluate(&local,parent<0?NULL:&worlds[parent],&worlds[k])==AYATSURI_OK);
  }
  for(size_t j=0;j<hone_wrap_route_count;j++){
   const hone_wrap_route *route=&hone_wrap_routes[j];size_t src=route->source_mesh,dst=route->target_mesh;
   assert(src<hone_input_mesh_count&&dst<hone_input_mesh_count&&src<hone_probe_mesh_count&&dst<hone_probe_mesh_count);
   assert(hone_probe_mesh_first_node[src]!=UINT32_MAX&&hone_probe_mesh_first_node[dst]!=UINT32_MAX);
   const hone_input_mesh *s=&hone_input_meshes[src],*t=&hone_input_meshes[dst];
   size_t np=s->source_vertex_count,nv=t->source_vertex_count,nt=t->triangles,nb=(nt+7)/8,box_bytes=hone_native_wrap_box_bytes(nt),tested=0;
   assert(np&&nv&&nt&&box_bytes);double matrix[12];
   assert(hone_native_relative_transform(worlds[hone_probe_mesh_first_node[dst]].m,worlds[hone_probe_mesh_first_node[src]].m,matrix));
   double *pts=malloc(np*3*sizeof *pts),*vertices=malloc(nv*3*sizeof *vertices),*hits=malloc(np*3*sizeof *hits);
   uint32_t *faces=malloc(np*sizeof *faces),*triangles=malloc(nt*3*sizeof *triangles);void *boxes=malloc(box_bytes);float *wrapped=malloc(np*3*sizeof *wrapped);
   assert(pts&&vertices&&hits&&faces&&triangles&&boxes&&wrapped);
   for(size_t k=0;k<nt*3;k++){uint32_t prepared=t->index[k];assert(prepared<t->vertices);triangles[k]=t->source_vertex[prepared];assert(triangles[k]<nv);}
   assert(hone_native_wrap(positions[src],np,positions[dst],nv,triangles,nt,matrix,route->offset,pts,np*3,vertices,nv*3,hits,np*3,faces,np,boxes,nb,wrapped,np*3,&tested));
   memcpy(positions[src],wrapped,np*3*sizeof *wrapped);free(pts);free(vertices);free(hits);free(faces);free(triangles);free(boxes);free(wrapped);
  }
  if(position_file)for(size_t k=0;k<hone_input_mesh_count;k++){
   size_t count=(size_t)hone_input_meshes[k].source_vertex_count*3;
   assert(fwrite(positions[k],sizeof(float),count,position_file)==count);
  }
#endif
#ifdef HONE_PROBE_MORPH_DRIVER
  for(size_t k=0;k<hone_input_mesh_count;k++){
   const hone_input_mesh *msh=&hone_input_meshes[k];
   assert(polygon_normals(positions[k],msh->source_vertex_count,msh->polygon_offsets,msh->polygon_vertices,msh->polygon_smooth,msh->polygon_count,msh->polygon_corner_count,work,(float*)meshes[k].packed_normals));
   meshes[k].source_positions=positions[k];
#ifdef HONE_PROBE_WRAP
   if(normal_file){size_t count=(size_t)msh->polygon_corner_count*3;assert(fwrite(meshes[k].packed_normals,sizeof(float),count,normal_file)==count);}
#endif
  }
#endif
  ayatsuri_model_targets targets={dirty,pose,colors,opacity,m.node_count};
  assert(ayatsuri_model_instance_targets(&inst,&owner,1,&targets)==AYATSURI_MODEL_OK);
  unsigned before=ctx.calls;
  assert(ayatsuri_model_candidate_evaluate_provider(&inst,&owner,1,camera,light,provider,&ctx,slot)==AYATSURI_MODEL_OK);
  assert(ctx.calls-before==expected_calls);
 }
#ifdef HONE_PROBE_MORPH_DRIVER
#ifdef HONE_PROBE_WRAP
 printf("PASS node, morph, sampled-transform root rig and wrap frames=4 provider_calls=%u materials=placeholder runtime_ready=false\n",ctx.calls);
#elif defined(HONE_PROBE_RIG_DRIVER)
 printf("PASS node, unmasked morph and sampled-transform root rig frames=4 provider_calls=%u shrinkwrap_material=unapplied runtime_ready=false\n",ctx.calls);
#else
 printf("PASS node and unmasked morph driver frames=4 provider_calls=%u rig_shrinkwrap_material=unapplied runtime_ready=false\n",ctx.calls);
#endif
 free(morphs);
#else
 printf("PASS node driver frames=4 provider_calls=%u rig_morph_material_outputs=unapplied runtime_ready=false\n",ctx.calls);
#endif
 free(inputs);free(scratch);free(values);free(opacity);free(pose);free(dirty);free(colors);
#ifdef HONE_PROBE_RIG_DRIVER
 free(bone_scratch);free(palette_scratch);free(palette);
#endif
#ifdef HONE_PROBE_WRAP
 if(position_file)assert(!fclose(position_file));if(normal_file)assert(!fclose(normal_file));free(worlds);
#endif
#endif
 assert(ayatsuri_model_instance_dispose(&inst,&owner,1)==AYATSURI_MODEL_OK);
 for(size_t i=0;i<hone_input_mesh_count;i++)free((void*)meshes[i].packed_normals);
#ifdef HONE_PROBE_MORPH_DRIVER
 for(size_t i=0;i<hone_input_mesh_count;i++){free(positions[i]);free(masks[i]);}free(positions);free(masks);
#endif
 free(work);free(meshes);free(arena);free(blob);return 0;
}
