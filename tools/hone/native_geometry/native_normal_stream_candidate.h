#ifndef HONE_NATIVE_NORMAL_STREAM_CANDIDATE_H
#define HONE_NATIVE_NORMAL_STREAM_CANDIDATE_H
#include "native_provider.h"
/* Independent lazy normal provider. Generated mesh/node tables and deformed
 * positions are borrowed immutable for a whole frame. One normal buffer is
 * exclusively owned, disjoint from source/model/snapshot/slot and callback work.
 * begin must precede EVERY new deformation frame, including identical controls.
 * No retained normal view, no allocation, publication or display operations. */
typedef bool (*hone_normal_stream_compute)(void*,const hone_input_mesh*,const float*,float*);
typedef struct {
 const hone_provider_context *source;
 float *normals;size_t normal_floats;
 hone_normal_stream_compute compute;void*compute_context;
 uint64_t frame;size_t mesh;bool valid,busy;
} hone_normal_stream;
static inline bool hone_normal_stream_begin(hone_normal_stream*s,uint64_t frame){
 if(!s||s->busy||!frame||frame<=s->frame)return false;
 s->valid=false;s->frame=frame;return true;
}
static inline bool hone_normal_stream_provider(void*opaque,unsigned node,unsigned count,ayatsuri_skin_result*slot){
 hone_normal_stream*s=opaque;
 if(!s||s->busy||!s->frame||!s->source||!s->normals||!s->compute||!slot||count>32)return false;
 const hone_provider_context*c=s->source;
 if(!c->meshes||!c->nodes||node>=c->node_count)return false;
 const hone_provider_part*p=&c->nodes[node];if(p->count!=count||p->mesh_index>=c->mesh_count)return false;
 const hone_provider_mesh*m=&c->meshes[p->mesh_index];if(!m->mesh||!m->source_positions||(size_t)m->mesh->polygon_corner_count>s->normal_floats/3||s->normal_floats>SIZE_MAX/sizeof(float))return false;
 uintptr_t a=(uintptr_t)s->normals,b=(uintptr_t)slot;size_t bytes=s->normal_floats*sizeof(float),out=count*sizeof *slot;
 if(bytes>UINTPTR_MAX-a||out>UINTPTR_MAX-b||!(a+bytes<=b||b+out<=a))return false;
 if(!s->valid||s->mesh!=p->mesh_index){
  s->valid=false;s->busy=true;bool ok=s->compute(s->compute_context,m->mesh,m->source_positions,s->normals);s->busy=false;
  if(!ok)return false;s->mesh=p->mesh_index;s->valid=true;
 }
 hone_gather_index plan[32];
 if(!hone_gather_plan(m->mesh,p->prepared_vertices,count,m->source_positions,s->normals,plan))return false;
 for(unsigned j=0;j<count;j++)for(unsigned k=0;k<3;k++){
  slot[j].position[k]=m->source_positions[3*(size_t)plan[j].source+k];
  slot[j].normal[k]=s->normals[3*(size_t)plan[j].normal+k];
 }
 return true;
}
#endif
