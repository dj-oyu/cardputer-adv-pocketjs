#ifndef HONE_BOUNDS_FRAME_CANDIDATE_H
#define HONE_BOUNDS_FRAME_CANDIDATE_H
#include "frame_lane_candidate.h"
#include "ui/kasane/ksn_mesh_depth.h"
/* Separate host candidate: freezes the original frame-lane algorithm with
 * conservative integer bounds. Existing/default entry points are unchanged.
 * Ready and pending frames require separate bounds buffers; never rebuild a
 * ready cache in place. Caller owns disjoint bounds storage, immutable from successful preflight
 * through final ACK/unpin, just like packet storage. Not target admission. */
typedef struct {uint8_t x0,x1,y0,y1;} hone_alpha_bounds;
static inline hone_alpha_bounds hone_bounds_make(const hone_alpha_packet*p){
 float x0=p->v[0].x,x1=x0,y0=p->v[0].y,y1=y0;
 for(unsigned i=1;i<3;i++){x0=fminf(x0,p->v[i].x);x1=fmaxf(x1,p->v[i].x);y0=fminf(y0,p->v[i].y);y1=fmaxf(y1,p->v[i].y);}
 if(x1<0||x0>239||y1<0||y0>134)return (hone_alpha_bounds){1,0,1,0};
 return (hone_alpha_bounds){(uint8_t)ceilf(fmaxf(0,x0)),(uint8_t)floorf(fminf(239,x1)),(uint8_t)ceilf(fmaxf(0,y0)),(uint8_t)floorf(fminf(134,y1))};
}
static inline int hone_bounds_fragments(const hone_alpha_snapshot*s,const hone_alpha_bounds*b,unsigned x,unsigned y,float opaque,hone_alpha_fragment f[4]){
 unsigned n=0;for(size_t i=0;i<s->count;i++){
  if(x<b[i].x0||x>b[i].x1||y<b[i].y0||y>b[i].y1)continue;
  hone_alpha_fragment q;if(!hone_alpha_sample(s->packets+i,x,y,opaque,&q))continue;
  if(n==4)return -1;unsigned at=n;while(at&&(f[at-1].z>q.z||(f[at-1].z==q.z&&f[at-1].order<=q.order))){f[at]=f[at-1];at--;}f[at]=q;n++;
 }return (int)n;
}
/* Trusted synchronous candidate owner. No user callbacks, no retained source
 * beyond frame-owned snapshots. Caller-owned band output and 960-byte depth scratch. */
typedef struct {const ksn_mesh_snapshot *opaque;hone_alpha_snapshot *alpha;const void*owner;uint32_t generation;int validated,pinned;uint64_t alpha_revision;const void *opaque_data;size_t opaque_size;hone_alpha_bounds *bounds;size_t bounds_capacity;const hone_alpha_packet *bound_packets;size_t bound_count;} hone_bounds_frame_lane;
static inline int hone_bounds_frame_preflight(hone_bounds_frame_lane*f,const void*owner,uint32_t generation,unsigned span_limit){
 if(!f||!owner||f->owner!=owner||f->generation!=generation||!generation||f->pinned||!f->opaque||!f->opaque->enabled||!f->alpha||!span_limit||span_limit>240)return 0;
 const hone_alpha_snapshot*a=f->alpha;
 if(a->owner!=owner||a->generation!=generation||!a->ready||a->pinned||a->count>a->capacity||a->count>65536||(a->count&&!a->packets))return 0;
 if(!hone_alpha_disjoint(f,sizeof *f,a,sizeof *a)||!hone_alpha_disjoint(f,sizeof *f,f->opaque,sizeof *f->opaque)||!hone_alpha_disjoint(a,sizeof *a,f->opaque,sizeof *f->opaque))return 0;
 if(a->count&&(!hone_alpha_disjoint(f,sizeof *f,a->packets,a->count*sizeof *a->packets)||!hone_alpha_disjoint(a,sizeof *a,a->packets,a->count*sizeof *a->packets)))return 0;
 if(!hone_alpha_disjoint(f,sizeof *f,f->opaque->packets.data,f->opaque->packets.size)||!hone_alpha_disjoint(a,sizeof *a,f->opaque->packets.data,f->opaque->packets.size))return 0;
 size_t bounds_bytes=a->count*sizeof *f->bounds;
 if(bounds_bytes&&(!hone_alpha_disjoint(f->bounds,bounds_bytes,f,sizeof *f)||!hone_alpha_disjoint(f->bounds,bounds_bytes,a,sizeof *a)||!hone_alpha_disjoint(f->bounds,bounds_bytes,f->opaque,sizeof *f->opaque)||!hone_alpha_disjoint(f->bounds,bounds_bytes,a->packets,a->count*sizeof *a->packets)||!hone_alpha_disjoint(f->bounds,bounds_bytes,f->opaque->packets.data,f->opaque->packets.size)))return 0;
 f->validated=0;
 if(a->count>f->bounds_capacity||(a->count&&!f->bounds)||f->bounds_capacity>SIZE_MAX/sizeof *f->bounds)return 0;

 for(size_t i=0;i<a->count;i++){hone_alpha_packet tmp[3];if(hone_alpha_clip(a->packets+i,-1e20f,1e20f,tmp)<0)return 0;for(unsigned j=0;j<3;j++)if(fabsf(a->packets[i].v[j].x)>1e6f||fabsf(a->packets[i].v[j].y)>1e6f||fabsf(a->packets[i].v[j].z)>1e10f||fabsf(a->packets[i].v[j].mask)>1e10f)return 0;}
 /* Every alpha packet has passed finite/range validation before any bounds write. */
 for(size_t i=0;i<a->count;i++)f->bounds[i]=hone_bounds_make(a->packets+i);
 /* Validate opaque packet topology without rasterizing or shading it. */
 size_t offset=0;while(offset<f->opaque->packets.size){
  co_view view;if(!co_read(f->opaque->packets.data+offset,f->opaque->packets.size-offset,&view)||!view.size)return 0;
  for(unsigned face=0;face<view.triangle_count;face++){
   uint8_t index[3];co_vertex v[3];bool back;
   if(!co_triangle_at(&view,face,index)||!co_triangle_side(&view,face,&back))return 0;
   for(unsigned j=0;j<3;j++)if(!co_vertex_at_side(&view,index[j],back,v+j)||!isfinite(v[j].depth))return 0;
   double area=((double)v[1].x-v[0].x)*((double)v[2].y-v[0].y)-((double)v[1].y-v[0].y)*((double)v[2].x-v[0].x);
   if(!(area>1e-5))return 0;
  }
  offset+=view.size;
 }
 /* Conservative transparent-only guarantee: opaque occlusion can only remove
  * fragments, never add them. This avoids ANY opaque pixel preflight scan,
  * but may reject scenes that would fit only after opaque occlusion. */
 for(unsigned y=0;y<135&&a->count;y++){
  unsigned left=240,right=0;
  for(unsigned x=0;x<240;x++){hone_alpha_fragment fragments[4];int n=hone_bounds_fragments(a,f->bounds,x,y,-INFINITY,fragments);if(n<0)return 0;if(n){if(x<left)left=x;right=x+1;}}
  if(right&&right-left>span_limit)return 0;
 }
 f->bound_packets=a->packets;f->bound_count=a->count;f->alpha_revision=a->revision;f->opaque_data=f->opaque->packets.data;f->opaque_size=f->opaque->packets.size;f->validated=1;return 1;
}
/* Whole frame preflight must have succeeded before FIRST LCD submission.
 * Keep both snapshots immutable/pinned throughout every band and final ACK.
 * Output belongs to caller's existing band; no full-frame buffer is added. */
static inline int hone_bounds_frame_band(const hone_bounds_frame_lane*f,const void*owner,uint32_t generation,uint16_t*pixels,unsigned y,unsigned height,float*depth){
 if(!f||f->owner!=owner||f->generation!=generation||!f->validated||f->alpha->packets!=f->bound_packets||f->alpha->count!=f->bound_count||!f->pinned||!f->alpha->pinned||f->alpha->revision!=f->alpha_revision||f->opaque->packets.data!=f->opaque_data||f->opaque->packets.size!=f->opaque_size||!pixels||!depth||y>135||height>135-y)return 0;
 size_t bytes=(size_t)height*240*sizeof *pixels;
 if(!hone_alpha_disjoint(pixels,bytes,f->bounds,f->bound_count*sizeof *f->bounds)||!hone_alpha_disjoint(depth,240*sizeof *depth,f->bounds,f->bound_count*sizeof *f->bounds))return 0;
 if(!hone_alpha_disjoint(pixels,bytes,depth,240*sizeof *depth)||!hone_alpha_disjoint(pixels,bytes,f,sizeof *f)||!hone_alpha_disjoint(pixels,bytes,f->opaque->packets.data,f->opaque->packets.size)||!hone_alpha_disjoint(pixels,bytes,f->alpha,sizeof *f->alpha)||!hone_alpha_disjoint(pixels,bytes,f->opaque,sizeof *f->opaque))return 0;
 if(f->alpha->count&&!hone_alpha_disjoint(pixels,bytes,f->alpha->packets,f->alpha->count*sizeof *f->alpha->packets))return 0;
 if(!hone_alpha_disjoint(depth,240*sizeof *depth,f,sizeof *f)||!hone_alpha_disjoint(depth,240*sizeof *depth,f->opaque,sizeof *f->opaque)||!hone_alpha_disjoint(depth,240*sizeof *depth,f->alpha,sizeof *f->alpha)||!hone_alpha_disjoint(depth,240*sizeof *depth,f->opaque->packets.data,f->opaque->packets.size))return 0;
 if(f->alpha->count&&!hone_alpha_disjoint(depth,240*sizeof *depth,f->alpha->packets,f->alpha->count*sizeof *f->alpha->packets))return 0;
 if(!f->alpha->count)return ksn_mesh_render(f->opaque,pixels,y,height,depth,1);
 for(unsigned yy=y;yy<y+height;yy++){
  uint16_t*row=pixels+(yy-y)*240;if(!ksn_mesh_render(f->opaque,row,yy,1,depth,1))return 0;
  for(unsigned x=0;x<240;x++){hone_alpha_fragment fragments[4];int n=hone_bounds_fragments(f->alpha,f->bounds,x,yy,depth[x],fragments);if(n<0)return 0;if(!n)continue;
   float rgb[3]={(row[x]>>11)/31.f,((row[x]>>5)&63)/63.f,(row[x]&31)/31.f};
   for(int i=0;i<n;i++)for(unsigned k=0;k<3;k++)rgb[k]=fragments[i].rgb[k]*fragments[i].alpha+rgb[k]*(1-fragments[i].alpha);
   row[x]=(uint16_t)(((unsigned)floorf(rgb[0]*31+.5f)<<11)|((unsigned)floorf(rgb[1]*63+.5f)<<5)|(unsigned)floorf(rgb[2]*31+.5f));
  }
 }
 return 1;
}
/* Publish one descriptor for both lanes, not two independent visible swaps.
 * Packet ownership stays with caller until all bands finish and ACK unpins. */
static inline int hone_bounds_frame_commit(hone_bounds_frame_lane*ready,const hone_bounds_frame_lane*candidate,const void*owner,uint32_t generation){
 if(!ready||!candidate||ready==candidate||!owner||ready->owner!=owner||ready->pinned||candidate->owner!=owner||candidate->generation!=generation||!candidate->validated||candidate->alpha->packets!=candidate->bound_packets||candidate->alpha->count!=candidate->bound_count||candidate->pinned||candidate->alpha->pinned||candidate->alpha->revision!=candidate->alpha_revision||candidate->opaque->packets.data!=candidate->opaque_data||candidate->opaque->packets.size!=candidate->opaque_size)return 0;
 *ready=*candidate;return 1;
}
/* Pin the complete pending-ready frame before first band. Unpin only after
 * final ACK/drain; committed-visible promotion belongs to presentation ACK. */
static inline int hone_bounds_frame_pin(hone_bounds_frame_lane*f,const void*owner,uint32_t generation){
 if(!f||f->owner!=owner||!owner||f->generation!=generation||!generation||!f->validated||f->pinned||!f->alpha||f->alpha->packets!=f->bound_packets||f->alpha->count!=f->bound_count||f->alpha->pinned||f->alpha->owner!=owner||f->alpha->generation!=generation||f->alpha->revision!=f->alpha_revision||f->opaque->packets.data!=f->opaque_data||f->opaque->packets.size!=f->opaque_size)return 0;
 f->alpha->pinned=1;f->pinned=1;return 1;
}
static inline int hone_bounds_frame_unpin(hone_bounds_frame_lane*f,const void*owner,uint32_t generation){
 if(!f||f->owner!=owner||!owner||f->generation!=generation||!generation||!f->pinned||!f->alpha||!f->alpha->pinned)return 0;
 f->alpha->pinned=0;f->pinned=0;return 1;
}

/* Preferred synchronous entry. Revision capture also detects same-generation
 * legal alpha republish between separately invoked preflight and pin. */
static inline int hone_bounds_frame_preflight_pin(hone_bounds_frame_lane*f,const void*owner,uint32_t generation,unsigned span_limit){
 return hone_bounds_frame_preflight(f,owner,generation,span_limit)&&hone_bounds_frame_pin(f,owner,generation);
}

#endif
