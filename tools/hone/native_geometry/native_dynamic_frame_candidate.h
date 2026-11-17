#ifndef HONE_NATIVE_DYNAMIC_FRAME_CANDIDATE_H
#define HONE_NATIVE_DYNAMIC_FRAME_CANDIDATE_H
#include "native_normal_stream_candidate.h"
#include <string.h>
#include <math.h>
_Static_assert(sizeof(float)==4 && sizeof(double)==8,"32-bit float / 64-bit double storage required");
/* Cold geometry/tables borrowed immutable. Arena owns only transient geometry.
 * No arena pointer may escape into retained snapshot/material payload storage.
 * Returned mutable position views expire at seal/begin/dispose. */
typedef bool (*hone_dynamic_normal_fn)(void*,const hone_input_mesh*,const float*,double*,size_t,float*);
typedef struct {
 const void*owner;uint32_t generation;uint64_t revision;bool alive,sealed,busy;
 void*arena;size_t arena_bytes;const hone_input_mesh*cold;size_t mesh_count;
 hone_provider_mesh*meshes;hone_provider_context context;hone_normal_stream normal;
 double*normal_work;size_t normal_work_count;hone_dynamic_normal_fn compute;void*compute_context;
} hone_dynamic_frame;
static inline bool hone_dynamic_disjoint(const void*a,size_t an,const void*b,size_t bn){
 uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;return an<=UINTPTR_MAX-x&&bn<=UINTPTR_MAX-y&&(x+an<=y||y+bn<=x);
}
static inline size_t hone_dynamic_frame_bytes(const hone_input_mesh*cold,size_t count){
 if(!cold||!count||count>256)return 0;size_t positions=0,maxv=0,maxc=0;
 for(size_t i=0;i<count;i++){
  size_t v=cold[i].source_vertex_count,c=cold[i].polygon_corner_count;
  if(v>1000000||c>1000000||(v&&!cold[i].source_position)||v>(SIZE_MAX-positions)/12)return 0;
  positions+=12*v;if(v>maxv)maxv=v;if(c>maxc)maxc=c;
 }
 size_t descriptors=count*sizeof(hone_provider_mesh),total=descriptors+positions+12*maxc;
 if(total>SIZE_MAX-7)return 0;total=(total+7)&~(size_t)7;
 if(maxv>(SIZE_MAX-total)/24)return 0;return total+24*maxv;
}
static inline bool hone_dynamic_normal(void*opaque,const hone_input_mesh*m,const float*p,float*n){
 hone_dynamic_frame*f=opaque;return f->compute(f->compute_context,m,p,f->normal_work,f->normal_work_count,n);
}
/* f starts zeroed; arena is 8-byte aligned, exclusive and persistent. All other
 * callback-owned state/cold arrays also remain disjoint from this entire arena. */
static inline bool hone_dynamic_frame_init(hone_dynamic_frame*f,const void*owner,uint32_t generation,
 const hone_input_mesh*cold,size_t mesh_count,const hone_provider_part*nodes,size_t node_count,
 void*arena,size_t capacity,hone_dynamic_normal_fn compute,void*compute_context){
 size_t need=hone_dynamic_frame_bytes(cold,mesh_count);
 if(!f||f->alive||!owner||!generation||!need||!nodes||!node_count||node_count>256||!arena||((uintptr_t)arena%8)||capacity<need||!compute||
  !hone_dynamic_disjoint(f,sizeof *f,arena,capacity)||!hone_dynamic_disjoint(f,sizeof *f,cold,mesh_count*sizeof *cold)||
  !hone_dynamic_disjoint(f,sizeof *f,nodes,node_count*sizeof *nodes)||!hone_dynamic_disjoint(arena,capacity,cold,mesh_count*sizeof *cold)||
  !hone_dynamic_disjoint(arena,capacity,nodes,node_count*sizeof *nodes))return false;
 for(size_t i=0;i<mesh_count;i++)if(!hone_dynamic_disjoint(arena,capacity,cold[i].source_position,12*(size_t)cold[i].source_vertex_count)||
  !hone_dynamic_disjoint(f,sizeof *f,cold[i].source_position,12*(size_t)cold[i].source_vertex_count))return false;
 for(size_t i=0;i<node_count;i++)if(nodes[i].count&&(nodes[i].mesh_index>=mesh_count||nodes[i].count>32||!nodes[i].prepared_vertices||
  !hone_dynamic_disjoint(arena,capacity,nodes[i].prepared_vertices,nodes[i].count*sizeof(uint32_t))))return false;
 hone_dynamic_frame value={.owner=owner,.generation=generation,.alive=true,.arena=arena,.arena_bytes=capacity,.cold=cold,.mesh_count=mesh_count,.meshes=arena,.compute=compute,.compute_context=compute_context};
 size_t offset=mesh_count*sizeof(hone_provider_mesh),maxv=0,maxc=0;
 for(size_t i=0;i<mesh_count;i++){
  value.meshes[i]=(hone_provider_mesh){cold+i,(float*)((uint8_t*)arena+offset),NULL};offset+=12*(size_t)cold[i].source_vertex_count;
  if(cold[i].source_vertex_count>maxv)maxv=cold[i].source_vertex_count;if(cold[i].polygon_corner_count>maxc)maxc=cold[i].polygon_corner_count;
 }
 float*normals=(float*)((uint8_t*)arena+offset);offset+=12*maxc;offset=(offset+7)&~(size_t)7;
 value.normal_work=(double*)((uint8_t*)arena+offset);value.normal_work_count=3*maxv;
 value.context=(hone_provider_context){value.meshes,mesh_count,nodes,node_count};
 value.normal=(hone_normal_stream){.normals=normals,.normal_floats=3*maxc,.compute=hone_dynamic_normal,.compute_context=f};
 *f=value;f->normal.source=&f->context;return true;
}
static inline bool hone_dynamic_owned(const hone_dynamic_frame*f,const void*owner,uint32_t gen){return f&&f->alive&&!f->busy&&owner&&f->owner==owner&&gen&&f->generation==gen;}
static inline bool hone_dynamic_frame_begin(hone_dynamic_frame*f,const void*owner,uint32_t gen,uint64_t revision){
 if(!hone_dynamic_owned(f,owner,gen)||!revision||revision<=f->revision)return false;
 f->sealed=false;f->revision=revision;f->normal.valid=false;
 for(size_t i=0;i<f->mesh_count;i++)if(f->cold[i].source_vertex_count)memcpy((void*)f->meshes[i].source_positions,f->cold[i].source_position,12*(size_t)f->cold[i].source_vertex_count);
 return true;
}
static inline float*hone_dynamic_frame_positions(hone_dynamic_frame*f,const void*owner,uint32_t gen,size_t mesh){
 return hone_dynamic_owned(f,owner,gen)&&f->revision&&!f->sealed&&mesh<f->mesh_count?(float*)f->meshes[mesh].source_positions:NULL;
}
static inline bool hone_dynamic_frame_seal(hone_dynamic_frame*f,const void*owner,uint32_t gen){
 if(!hone_dynamic_owned(f,owner,gen)||!f->revision||f->sealed)return false;
 for(size_t i=0;i<f->mesh_count;i++)for(size_t j=0;j<3*(size_t)f->cold[i].source_vertex_count;j++)if(!isfinite(f->meshes[i].source_positions[j]))return false;
 if(!hone_normal_stream_begin(&f->normal,f->revision))return false;f->sealed=true;return true;
}
static inline bool hone_dynamic_frame_provider(void*opaque,unsigned node,unsigned count,ayatsuri_skin_result*slot){
 hone_dynamic_frame*f=opaque;if(!f||!f->alive||!f->sealed||f->busy||!slot||count>32||
  !hone_dynamic_disjoint(slot,count*sizeof *slot,f,sizeof *f)||!hone_dynamic_disjoint(slot,count*sizeof *slot,f->arena,f->arena_bytes))return false;
 f->busy=true;bool ok=hone_normal_stream_provider(&f->normal,node,count,slot);f->busy=false;return ok;
}
static inline bool hone_dynamic_frame_dispose(hone_dynamic_frame*f,const void*owner,uint32_t gen){
 if(!hone_dynamic_owned(f,owner,gen))return false;memset(f,0,sizeof *f);return true;
}
#endif
