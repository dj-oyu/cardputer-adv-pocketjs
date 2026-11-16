#ifndef HONE_FRAME_LANE_CANDIDATE_H
#define HONE_FRAME_LANE_CANDIDATE_H
#include "alpha_raster_candidate.h"
#include "ui/kasane/ksn_mesh_depth.h"
/* Trusted synchronous candidate owner. No user callbacks, no retained source
 * beyond frame-owned snapshots. Caller-owned band output and 960-byte depth scratch. */
typedef struct {const ksn_mesh_snapshot *opaque;hone_alpha_snapshot *alpha;const void*owner;uint32_t generation;int validated,pinned;uint64_t alpha_revision;const void *opaque_data;size_t opaque_size;} hone_frame_lane;
static inline int hone_frame_preflight(hone_frame_lane*f,const void*owner,uint32_t generation,unsigned span_limit){
 if(!f||!owner||f->owner!=owner||f->generation!=generation||!generation||f->pinned||!f->opaque||!f->opaque->enabled||!f->alpha||!span_limit||span_limit>240)return 0;
 const hone_alpha_snapshot*a=f->alpha;
 if(a->owner!=owner||a->generation!=generation||!a->ready||a->pinned||a->count>a->capacity||a->count>65536||(a->count&&!a->packets))return 0;
 if(!hone_alpha_disjoint(f,sizeof *f,a,sizeof *a)||!hone_alpha_disjoint(f,sizeof *f,f->opaque,sizeof *f->opaque)||!hone_alpha_disjoint(a,sizeof *a,f->opaque,sizeof *f->opaque))return 0;
 if(a->count&&(!hone_alpha_disjoint(f,sizeof *f,a->packets,a->count*sizeof *a->packets)||!hone_alpha_disjoint(a,sizeof *a,a->packets,a->count*sizeof *a->packets)))return 0;
 if(!hone_alpha_disjoint(f,sizeof *f,f->opaque->packets.data,f->opaque->packets.size)||!hone_alpha_disjoint(a,sizeof *a,f->opaque->packets.data,f->opaque->packets.size))return 0;
 f->validated=0;
 for(size_t i=0;i<a->count;i++){hone_alpha_packet tmp[3];if(hone_alpha_clip(a->packets+i,-1e20f,1e20f,tmp)<0)return 0;for(unsigned j=0;j<3;j++)if(fabsf(a->packets[i].v[j].x)>1e6f||fabsf(a->packets[i].v[j].y)>1e6f||fabsf(a->packets[i].v[j].z)>1e10f||fabsf(a->packets[i].v[j].mask)>1e10f)return 0;}
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
  for(unsigned x=0;x<240;x++){hone_alpha_fragment fragments[4];int n=hone_alpha_fragments(a,x,y,-INFINITY,fragments);if(n<0)return 0;if(n){if(x<left)left=x;right=x+1;}}
  if(right&&right-left>span_limit)return 0;
 }
 f->alpha_revision=a->revision;f->opaque_data=f->opaque->packets.data;f->opaque_size=f->opaque->packets.size;f->validated=1;return 1;
}
/* Whole frame preflight must have succeeded before FIRST LCD submission.
 * Keep both snapshots immutable/pinned throughout every band and final ACK.
 * Output belongs to caller's existing band; no full-frame buffer is added. */
static inline int hone_frame_band(const hone_frame_lane*f,const void*owner,uint32_t generation,uint16_t*pixels,unsigned y,unsigned height,float*depth){
 if(!f||f->owner!=owner||f->generation!=generation||!f->validated||!f->pinned||!f->alpha->pinned||f->alpha->revision!=f->alpha_revision||f->opaque->packets.data!=f->opaque_data||f->opaque->packets.size!=f->opaque_size||!pixels||!depth||y>135||height>135-y)return 0;
 size_t bytes=(size_t)height*240*sizeof *pixels;
 if(!hone_alpha_disjoint(pixels,bytes,depth,240*sizeof *depth)||!hone_alpha_disjoint(pixels,bytes,f,sizeof *f)||!hone_alpha_disjoint(pixels,bytes,f->opaque->packets.data,f->opaque->packets.size)||!hone_alpha_disjoint(pixels,bytes,f->alpha,sizeof *f->alpha)||!hone_alpha_disjoint(pixels,bytes,f->opaque,sizeof *f->opaque))return 0;
 if(f->alpha->count&&!hone_alpha_disjoint(pixels,bytes,f->alpha->packets,f->alpha->count*sizeof *f->alpha->packets))return 0;
 if(!hone_alpha_disjoint(depth,240*sizeof *depth,f,sizeof *f)||!hone_alpha_disjoint(depth,240*sizeof *depth,f->opaque,sizeof *f->opaque)||!hone_alpha_disjoint(depth,240*sizeof *depth,f->alpha,sizeof *f->alpha)||!hone_alpha_disjoint(depth,240*sizeof *depth,f->opaque->packets.data,f->opaque->packets.size))return 0;
 if(f->alpha->count&&!hone_alpha_disjoint(depth,240*sizeof *depth,f->alpha->packets,f->alpha->count*sizeof *f->alpha->packets))return 0;
 if(!f->alpha->count)return ksn_mesh_render(f->opaque,pixels,y,height,depth,1);
 for(unsigned yy=y;yy<y+height;yy++){
  uint16_t*row=pixels+(yy-y)*240;if(!ksn_mesh_render(f->opaque,row,yy,1,depth,1))return 0;
  for(unsigned x=0;x<240;x++){hone_alpha_fragment fragments[4];int n=hone_alpha_fragments(f->alpha,x,yy,depth[x],fragments);if(n<0)return 0;if(!n)continue;
   float rgb[3]={(row[x]>>11)/31.f,((row[x]>>5)&63)/63.f,(row[x]&31)/31.f};
   for(int i=0;i<n;i++)for(unsigned k=0;k<3;k++)rgb[k]=fragments[i].rgb[k]*fragments[i].alpha+rgb[k]*(1-fragments[i].alpha);
   row[x]=(uint16_t)(((unsigned)floorf(rgb[0]*31+.5f)<<11)|((unsigned)floorf(rgb[1]*63+.5f)<<5)|(unsigned)floorf(rgb[2]*31+.5f));
  }
 }
 return 1;
}
/* Publish one descriptor for both lanes, not two independent visible swaps.
 * Packet ownership stays with caller until all bands finish and ACK unpins. */
static inline int hone_frame_commit(hone_frame_lane*ready,const hone_frame_lane*candidate,const void*owner,uint32_t generation){
 if(!ready||!candidate||ready==candidate||!owner||ready->owner!=owner||ready->pinned||candidate->owner!=owner||candidate->generation!=generation||!candidate->validated||candidate->pinned||candidate->alpha->pinned||candidate->alpha->revision!=candidate->alpha_revision||candidate->opaque->packets.data!=candidate->opaque_data||candidate->opaque->packets.size!=candidate->opaque_size)return 0;
 *ready=*candidate;return 1;
}
/* Pin the complete pending-ready frame before first band. Unpin only after
 * final ACK/drain; committed-visible promotion belongs to presentation ACK. */
static inline int hone_frame_pin(hone_frame_lane*f,const void*owner,uint32_t generation){
 if(!f||f->owner!=owner||!owner||f->generation!=generation||!generation||!f->validated||f->pinned||!f->alpha||f->alpha->pinned||f->alpha->owner!=owner||f->alpha->generation!=generation||f->alpha->revision!=f->alpha_revision||f->opaque->packets.data!=f->opaque_data||f->opaque->packets.size!=f->opaque_size)return 0;
 f->alpha->pinned=1;f->pinned=1;return 1;
}
static inline int hone_frame_unpin(hone_frame_lane*f,const void*owner,uint32_t generation){
 if(!f||f->owner!=owner||!owner||f->generation!=generation||!generation||!f->pinned||!f->alpha||!f->alpha->pinned)return 0;
 f->alpha->pinned=0;f->pinned=0;return 1;
}

/* Preferred synchronous entry. Revision capture also detects same-generation
 * legal alpha republish between separately invoked preflight and pin. */
static inline int hone_frame_preflight_pin(hone_frame_lane*f,const void*owner,uint32_t generation,unsigned span_limit){
 return hone_frame_preflight(f,owner,generation,span_limit)&&hone_frame_pin(f,owner,generation);
}

#endif
