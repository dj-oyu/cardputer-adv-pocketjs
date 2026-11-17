#ifndef AY_STREAM_MATERIAL_PAYLOAD_CANDIDATE_H
#define AY_STREAM_MATERIAL_PAYLOAD_CANDIDATE_H
#include "ayatsuri/ayatsuri_stream_frame_owner_candidate.h"
#include "../../hone/native_geometry/alpha_graph_band_candidate.h"
#include "../../hone/native_geometry/alpha_graph_clip_candidate.h"
/* Trusted native, host-only admission experiment. Source hash must be checked
 * against actual source bytes by the exporter. Matching IDs are NOT authentication.
 * Programs/manifests remain immutable; copied packets/drivers belong to this frame. */
typedef struct {uint32_t polygon,material;} ay_material_face;
typedef struct {const ay_material_face *triangles;size_t count;} ay_material_mesh;
typedef struct {const hone_alpha_program *alpha,*color;} ay_material_program;
typedef struct {
 ayatsuri_stream_routes routes;
 const ay_material_mesh *meshes;
 const uint32_t *polygon_material;size_t polygon_count;
 const ay_material_program *materials;size_t material_count;
 const void *driver_binding;size_t driver_count;
 uint8_t source_hash[32];
} ay_material_package;
typedef struct {
 hone_graph_band_payload band;
 hone_graph_packet *packets;size_t capacity;
 double *drivers;size_t driver_capacity;
} ay_material_pending;
typedef struct {
 const ay_material_package *package;ay_material_pending *pending;
 const double *drivers;const void *binding;uint32_t generation;
 size_t cursor;bool started;
} ay_material_build;
static inline bool ay_material_observe(void*opaque,const ayatsuri_stream_attributed_mesh*v){
 ay_material_build*b=opaque;const ay_material_package*p=b->package;ay_material_pending*d=b->pending;
 if(b->cursor>=p->routes.count)return false;
 const ay_material_mesh*m=&p->meshes[b->cursor++];
 if(v->authored_triangle_count!=m->count||v->lane!=AYATSURI_STREAM_MATERIAL)return false;
 if(!b->started){
  d->band=(hone_graph_band_payload){.graph={d->packets,0,d->capacity,d->drivers,p->driver_count,b->binding,b->generation,0},.binding=b->binding,.generation=b->generation,.span_limit=240};
  if(p->driver_count)memcpy(d->drivers,b->drivers,p->driver_count*sizeof *d->drivers);
  b->started=true;
 }
 for(unsigned t=0;t<v->triangle_count;t++){
  unsigned original=v->source_triangle[t];if(original>=m->count)return false;
  ay_material_face face=m->triangles[original];const ay_material_program*program=&p->materials[face.material];
  hone_graph_triangle triangle={.alpha=program->alpha,.color=program->color};
  for(unsigned j=0;j<3;j++){
   unsigned k=v->indices[3*t+j];if(k>=v->vertex_count)return false;
   triangle.v[j]=(hone_graph_vertex){v->projected[k].x,v->projected[k].y,v->depth[k],{v->local[k].position[0],v->local[k].position[1],v->local[k].position[2]}};
  }
  hone_graph_triangle clipped[6];size_t n=0;
  if(!hone_graph_clip_viewport(&triangle,0,0,240,135,clipped,6,&n)||n>d->capacity-d->band.graph.count)return false;
  for(size_t j=0;j<n;j++){
   size_t at=d->band.graph.count++;d->packets[at]=(hone_graph_packet){clipped[j],(uint32_t)at};
  }
 }
 if(b->cursor==p->routes.count)d->band.graph.ready=1;
 return true;
}
/* All writable spans must be disjoint from every borrowed dependency. */
static inline bool ay_material_span(const ay_material_pending*d,const void*p,size_t n){
 if(!n)return true;if(!p||n>UINTPTR_MAX-(uintptr_t)p)return false;
 return ay_stream_owner_disjoint(d,sizeof *d,p,n)&&
  ay_stream_owner_disjoint(d->packets,d->capacity*sizeof *d->packets,p,n)&&
  ay_stream_owner_disjoint(d->drivers,d->driver_capacity*sizeof *d->drivers,p,n);
}
static inline ayatsuri_model_status ay_material_produce(ayatsuri_stream_frame_owner*f,const void*id,uint32_t gen,
 const float camera[9],const float light[6],ayatsuri_model_geometry_provider provider,void*context,
 ayatsuri_skin_result slot[32],const ay_material_package*p,const uint8_t source_hash[32],
 const void*driver_binding,const double*drivers,size_t driver_count,ay_material_pending*d){
 if(!ay_stream_owner_valid(f,id,gen)||f->pinned||f->revision==UINT64_MAX)return AYATSURI_MODEL_STATE;
 if(!p||!d||!source_hash||!driver_binding||p->driver_binding!=driver_binding||
    memcmp(p->source_hash,source_hash,32)||p->driver_count!=driver_count||driver_count>65536||
    !p->routes.count||p->routes.count>256||!p->routes.routes||!p->meshes||
    !p->polygon_count||p->polygon_count>65536||!p->polygon_material||
    !p->material_count||p->material_count>65536||!p->materials||
    !d->capacity||d->capacity>65536||!d->packets||d->driver_capacity>65536||d->driver_capacity<driver_count||
    (d->driver_capacity&&!d->drivers)||(driver_count&&!drivers)||
    !ay_stream_owner_disjoint(d,sizeof *d,d->packets,d->capacity*sizeof *d->packets)||
    !ay_stream_owner_disjoint(d,sizeof *d,d->drivers,d->driver_capacity*sizeof *d->drivers)||
    !ay_stream_owner_disjoint(d->packets,d->capacity*sizeof *d->packets,d->drivers,d->driver_capacity*sizeof *d->drivers))return AYATSURI_MODEL_INVALID;
 const void*read[]={f,f->stream,f->stream->core.arena,f->stream->core.model,f->stream->core.model->data,p,
  p->routes.routes,p->meshes,p->polygon_material,p->materials,drivers,slot,camera,light,source_hash};
 size_t sizes[]={sizeof *f,sizeof *f->stream,f->stream->core.arena_bytes,sizeof *f->stream->core.model,f->stream->core.model->bytes,sizeof *p,
  p->routes.count*sizeof *p->routes.routes,p->routes.count*sizeof *p->meshes,p->polygon_count*sizeof *p->polygon_material,
  p->material_count*sizeof *p->materials,driver_count*sizeof *drivers,32*sizeof *slot,9*sizeof *camera,6*sizeof *light,32};
 for(unsigned j=0;j<sizeof read/sizeof *read;j++)if(!ay_material_span(d,read[j],sizes[j]))return AYATSURI_MODEL_INVALID;
 for(unsigned j=0;j<2;j++)if(!ay_material_span(d,f->snapshots[j].packets.data,f->snapshots[j].packets.capacity))return AYATSURI_MODEL_INVALID;
 /* Payloads created by this API have this exact type. Caller must not mix an
  * arbitrary payload format into this owner. Keep both backing arenas exclusive. */
 if(f->ready){const ay_material_pending*old=f->payload[f->committed];
  if(!old||!ay_material_span(d,old,sizeof *old)||!ay_material_span(d,old->packets,old->capacity*sizeof *old->packets)||
     !ay_material_span(d,old->drivers,old->driver_capacity*sizeof *old->drivers))return AYATSURI_MODEL_INVALID;
 }
 for(size_t j=0;j<driver_count;j++)if(!isfinite(drivers[j]))return AYATSURI_MODEL_INVALID;
 for(size_t j=0;j<p->routes.count;j++){
  const ay_material_mesh*m=&p->meshes[j];if(p->routes.routes[j].lane!=AYATSURI_STREAM_MATERIAL||m->count>64||
   (m->count&&!m->triangles)||!ay_material_span(d,m->triangles,m->count*sizeof *m->triangles))return AYATSURI_MODEL_INVALID;
  for(size_t k=0;k<m->count;k++){ay_material_face face=m->triangles[k];if(face.polygon>=p->polygon_count||face.material>=p->material_count||p->polygon_material[face.polygon]!=face.material)return AYATSURI_MODEL_INVALID;}
 }
 for(size_t j=0;j<p->material_count;j++){
  const ay_material_program*q=&p->materials[j];if(!q->alpha||!q->color||!ay_material_span(d,q->alpha,sizeof *q->alpha)||!ay_material_span(d,q->color,3*sizeof *q->color))return AYATSURI_MODEL_INVALID;
  for(unsigned k=0;k<4;k++){const hone_alpha_program*a=k?q->color+k-1:q->alpha;
   if(!a->code||!a->count||a->count>1024||a->result>=a->count||!ay_material_span(d,a->code,a->count*sizeof *a->code))return AYATSURI_MODEL_INVALID;
  }
 }
 ay_material_build build={p,d,drivers,id,gen,0,false};
 return ayatsuri_stream_frame_produce_attributed(f,id,gen,camera,light,provider,context,slot,&p->routes,ay_material_observe,&build,d);
}
#endif
