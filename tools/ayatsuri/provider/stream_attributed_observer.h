/* Test-only observer shared by public fixtures and private host probes.
 * No asset values are logged. No callback pointer survives its invocation. */
#include "ayatsuri/ayatsuri_model_stream_candidate.h"
#include <assert.h>
#include <string.h>
static const ksn_mesh_snapshot *attributed_oracle;
static unsigned attributed_calls,attributed_hidden,attributed_faces;
static int attributed_fail=-1;
static uint16_t attr_u16(const uint8_t*p){return (uint16_t)(p[0]|((uint16_t)p[1]<<8));}
typedef struct {
 const ayatsuri_model *model;ayatsuri_model_geometry_provider provider;void *context;
 ayatsuri_skin_result copied[32];unsigned node,count;
} attributed_probe;
static bool attributed_provider(void*p,unsigned node,unsigned count,ayatsuri_skin_result*slot){
 attributed_probe*c=p;if(!c->provider(c->context,node,count,slot))return false;
 memcpy(c->copied,slot,count*sizeof *slot);c->node=node;c->count=count;return true;
}
static bool attributed_observe(void*p,const ayatsuri_stream_attributed_mesh*v){
 attributed_probe*c=p;attributed_calls++;
 assert(v->node_id==c->node&&v->vertex_count==c->count&&v->local&&v->projected&&v->depth);
 assert(!memcmp(v->local,c->copied,v->vertex_count*sizeof *v->local));
 if(!v->enabled){attributed_hidden++;assert(!v->triangle_count&&!v->indices&&!v->source_triangle);}
 const uint8_t*d=c->model->data+c->model->section[8].offset+32*v->mesh_id;
 assert(v->authored_triangle_count==attr_u16(d+6));bool used[64]={0};
 co_view reference={0};bool found=false;
 if(attributed_oracle)for(size_t off=0;off<attributed_oracle->packets.size;){
  co_view q;assert(co_read(attributed_oracle->packets.data+off,attributed_oracle->packets.size-off,&q));
  if(q.node==v->node_id){reference=q;found=true;break;}off+=q.size;
 }
 assert(!attributed_oracle||found==(v->triangle_count!=0));
 bool referenced[32]={0};unsigned remap[32]={0},compact_count=0;
 for(unsigned t=0;t<3*v->triangle_count;t++){assert(v->indices[t]<v->vertex_count);referenced[v->indices[t]]=true;}
 for(unsigned j=0;j<v->vertex_count;j++)if(referenced[j])remap[j]=compact_count++;
 if(found){
  assert(reference.vertex_count==compact_count&&reference.triangle_count==v->triangle_count);
  for(unsigned j=0;j<v->vertex_count;j++)if(referenced[j]){co_vertex q;assert(co_vertex_at(&reference,remap[j],&q));
   assert(q.x==v->projected[j].x&&q.y==v->projected[j].y&&q.depth==v->depth[j]);}
 }
 for(unsigned t=0;t<v->triangle_count;t++){
  unsigned original=v->source_triangle[t];assert(original<v->authored_triangle_count&&!used[original]);used[original]=true;
  const uint8_t*indices=c->model->data+c->model->section[10].offset+6*(attr_u16(d+4)+original);
  uint8_t q[3];if(found)assert(co_triangle_at(&reference,t,q));
  for(unsigned j=0;j<3;j++){assert(v->indices[3*t+j]==attr_u16(indices+2*j));if(found)assert(q[j]==remap[v->indices[3*t+j]]);}
  attributed_faces++;
 }
 return (int)v->node_id!=attributed_fail;
}
static size_t attributed_routes(const ayatsuri_model*m,ayatsuri_stream_route routes[256]){
 size_t count=0;
 for(unsigned j=0;j<m->node_count;j++){
  const uint8_t*n=m->data+m->section[0].offset+16*j;if(n[2]!=3)continue;
  routes[count++]=(ayatsuri_stream_route){(uint16_t)j,attr_u16(n+4),attr_u16(n+8),AYATSURI_STREAM_OPAQUE};
 }
 return count;
}
static ayatsuri_model_status attributed_probe_frame(ayatsuri_stream_candidate*s,const void*owner,uint32_t gen,
 const float*camera,const float*light,ayatsuri_model_geometry_provider provider,void*context,
 ayatsuri_skin_result*slot,ksn_mesh_snapshot*pending){
 ayatsuri_stream_route route[256];size_t count=attributed_routes(s->core.model,route);
 ayatsuri_stream_routes manifest={s->core.model,route,count};
 attributed_probe probe={.model=s->core.model,.provider=provider,.context=context};
 return ayatsuri_stream_candidate_frame_attributed(s,owner,gen,camera,light,attributed_provider,&probe,slot,pending,&manifest,attributed_observe,&probe);
}
