/* Separate opt-in TU. The ordinary model TU and ABI stay unchanged. */
#include "ayatsuri_model_stream_candidate.h"
#ifndef AYATSURI_MODEL_EXPERIMENTAL_PROVIDER
#define AYATSURI_MODEL_EXPERIMENTAL_PROVIDER
#endif
#define AYATSURI_MODEL_STREAM_INTERNAL
#define ayatsuri_model_open stream_internal_ayatsuri_model_open
#define ayatsuri_model_open_provider stream_internal_ayatsuri_model_open_provider
#define ayatsuri_model_find_node stream_internal_ayatsuri_model_find_node
#define ayatsuri_model_instance_bytes stream_internal_ayatsuri_model_instance_bytes
#define ayatsuri_model_instance_init stream_internal_ayatsuri_model_instance_init
#define ayatsuri_model_instance_dispose stream_internal_ayatsuri_model_instance_dispose
#define ayatsuri_model_instance_targets stream_internal_ayatsuri_model_instance_targets
#define ayatsuri_model_instance_node_kind stream_internal_ayatsuri_model_instance_node_kind
#define ayatsuri_model_instance_read_pose stream_internal_ayatsuri_model_instance_read_pose
#define ayatsuri_model_instance_stats stream_internal_ayatsuri_model_instance_stats
#define ayatsuri_model_instance_evaluate stream_internal_ayatsuri_model_instance_evaluate
#define ayatsuri_model_instance_evaluate_meshes stream_internal_ayatsuri_model_instance_evaluate_meshes
#define ayatsuri_model_instance_next stream_internal_ayatsuri_model_instance_next
#define ayatsuri_model_instance_mesh_view stream_internal_ayatsuri_model_instance_mesh_view
#define ayatsuri_model_instance_sided_mesh_view stream_internal_ayatsuri_model_instance_sided_mesh_view
#define ayatsuri_model_candidate_evaluate_provider stream_internal_ayatsuri_model_candidate_evaluate_provider
#include "ayatsuri_model.c"
#undef ayatsuri_model_open
#undef ayatsuri_model_open_provider
#undef ayatsuri_model_find_node
#undef ayatsuri_model_instance_bytes
#undef ayatsuri_model_instance_init
#undef ayatsuri_model_instance_dispose
#undef ayatsuri_model_instance_targets
#undef ayatsuri_model_instance_node_kind
#undef ayatsuri_model_instance_read_pose
#undef ayatsuri_model_instance_stats
#undef ayatsuri_model_instance_evaluate
#undef ayatsuri_model_instance_evaluate_meshes
#undef ayatsuri_model_instance_next
#undef ayatsuri_model_instance_mesh_view
#undef ayatsuri_model_instance_sided_mesh_view
#undef ayatsuri_model_candidate_evaluate_provider
#define STREAM_TAG UINT32_C(0x5354524d)
static bool stream_valid(const ayatsuri_stream_candidate*c){return c&&c->tag==STREAM_TAG&&c->core.alive&&c->core.model&&c->core.model->provider_only;}
ayatsuri_model_status ayatsuri_stream_candidate_open(const void*data,size_t bytes,ayatsuri_model*out){
 if(!out||overlaps(out,sizeof *out,data,bytes))return AYATSURI_MODEL_INVALID;
 ayatsuri_model candidate;
 ayatsuri_model_status status=stream_internal_ayatsuri_model_open_provider(data,bytes,&candidate);
 if(status!=AYATSURI_MODEL_OK)return status;
 if(!stream_internal_ayatsuri_model_instance_bytes(&candidate))return AYATSURI_MODEL_INVALID;
 *out=candidate;return AYATSURI_MODEL_OK;
}
size_t ayatsuri_stream_candidate_bytes(const ayatsuri_model*m){return stream_internal_ayatsuri_model_instance_bytes(m);}
ayatsuri_model_status ayatsuri_stream_candidate_init(const ayatsuri_model*m,void*arena,size_t bytes,const void*owner,uint32_t generation,ayatsuri_stream_candidate*out){
 size_t need=ayatsuri_stream_candidate_bytes(m);
 if(!out||out->tag||!need||overlaps(out,sizeof *out,arena,need)||overlaps(out,sizeof *out,m,sizeof *m)||overlaps(out,sizeof *out,m->data,m->bytes))return AYATSURI_MODEL_INVALID;
 ayatsuri_stream_candidate candidate={0};
 ayatsuri_model_status status=stream_internal_ayatsuri_model_instance_init(m,arena,bytes,owner,generation,&candidate.core);
 if(status!=AYATSURI_MODEL_OK)return status;
 candidate.tag=STREAM_TAG;*out=candidate;return AYATSURI_MODEL_OK;
}
ayatsuri_model_status ayatsuri_stream_candidate_targets(ayatsuri_stream_candidate*c,const void*owner,uint32_t generation,const ayatsuri_model_targets*t){
 if(!stream_valid(c))return AYATSURI_MODEL_STATE;
 return stream_internal_ayatsuri_model_instance_targets(&c->core,owner,generation,t);
}
ayatsuri_model_status ayatsuri_stream_candidate_dispose(ayatsuri_stream_candidate*c,const void*owner,uint32_t generation){
 if(!stream_valid(c))return AYATSURI_MODEL_STATE;
 ayatsuri_model_status status=stream_internal_ayatsuri_model_instance_dispose(&c->core,owner,generation);
 if(status==AYATSURI_MODEL_OK)c->tag=0;
 return status;
}
static bool stream_buffer_valid(const void*p,size_t bytes){return p&&bytes&&bytes<=UINTPTR_MAX-(uintptr_t)p;}
static bool stream_disjoint(const ayatsuri_stream_candidate*c,const void*p,size_t bytes){
 return !overlaps(p,bytes,c,sizeof *c)&&!overlaps(p,bytes,c->core.arena,c->core.arena_bytes)&&
 !overlaps(p,bytes,c->core.model,sizeof *c->core.model)&&!overlaps(p,bytes,c->core.model->data,c->core.model->bytes);
}
static bool stream_dispatch(void*opaque,unsigned node,unsigned count,ayatsuri_skin_result*slot){model_stream_request*r=opaque;return r->provider(r->user,node,count,slot);}
ayatsuri_model_status ayatsuri_stream_candidate_frame(ayatsuri_stream_candidate*c,const void*owner,uint32_t generation,
 const float camera[9],const float light[6],ayatsuri_model_geometry_provider provider,void*user,ayatsuri_skin_result slot[32],ksn_mesh_snapshot*pending){
 if(!stream_valid(c)||!owner||c->core.owner!=owner||!generation||c->core.generation!=generation)return AYATSURI_MODEL_STATE;
 if(c->core.busy)return AYATSURI_MODEL_BUSY;
 if(!provider||!camera||!light||!slot||!pending||!pending->enabled||
 !stream_buffer_valid(slot,32*sizeof *slot)||!stream_disjoint(c,slot,32*sizeof *slot)||
 !stream_buffer_valid(pending->packets.data,pending->packets.capacity)||
 !stream_disjoint(c,pending,sizeof *pending)||!stream_disjoint(c,pending->packets.data,pending->packets.capacity)||
 overlaps(pending,sizeof *pending,pending->packets.data,pending->packets.capacity)||
 overlaps(pending,sizeof *pending,slot,32*sizeof *slot)||overlaps(pending,sizeof *pending,camera,9*sizeof *camera)||overlaps(pending,sizeof *pending,light,6*sizeof *light)||
 overlaps(pending->packets.data,pending->packets.capacity,slot,32*sizeof *slot)||
 overlaps(pending->packets.data,pending->packets.capacity,camera,9*sizeof *camera)||overlaps(pending->packets.data,pending->packets.capacity,light,6*sizeof *light))return AYATSURI_MODEL_INVALID;
 model_stream_request request={provider,user,pending};
 pending->packets.size=pending->packets.packets=pending->work=0;
 return stream_internal_ayatsuri_model_candidate_evaluate_provider(&c->core,owner,generation,camera,light,stream_dispatch,&request,slot);
}
