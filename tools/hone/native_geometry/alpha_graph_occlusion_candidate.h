#ifndef HONE_ALPHA_GRAPH_OCCLUSION_CANDIDATE_H
#define HONE_ALPHA_GRAPH_OCCLUSION_CANDIDATE_H
#include "alpha_graph_raster_candidate.h"
/* Independent opt-in collector. Inputs and all dependencies are immutable and
 * scratch/output disjoint, as for the existing graph evaluator. No allocation.
 * Evaluate every fragment before discarding exactly opaque-covered contributions.
 * Existing depth/order and four RETAINED layer limit; no epsilon or alpha snapping.
 * Failure leaves out unchanged. This does not provide near clipping or lighting. */
static inline int hone_graph_fragments_occlusion(const hone_graph_snapshot*s,unsigned x,unsigned y,float depth,
 float*scratch,size_t scratch_count,hone_graph_ordered out[4]){
 if(!s||!out||s->count>s->capacity||s->count>65536||(s->count&&!s->packets)||
    s->driver_count>65536||(s->driver_count&&!s->drivers)||!scratch||!scratch_count||scratch_count>1024||
    isnan(depth)||depth==INFINITY)return -1;
 size_t opaque=SIZE_MAX;float opaque_z=0;uint32_t opaque_order=0;
 for(size_t i=0;i<s->count;i++){
  hone_graph_fragment f;int status=hone_graph_fragment_at(&s->packets[i].triangle,x,y,depth,s->drivers,s->driver_count,scratch,scratch_count,&f);
  if(status<0)return -1;
  /* Equal keys: the existing insertion sort places later array entries first,
   * hence the earliest entry is composed last and occludes equal-key successors. */
  if(status&&f.alpha==1&&(opaque==SIZE_MAX||f.z>opaque_z||
     (f.z==opaque_z&&s->packets[i].order<opaque_order))){
   opaque=i;opaque_z=f.z;opaque_order=s->packets[i].order;
  }
 }
 hone_graph_ordered staged[4];unsigned n=0;
 for(size_t i=0;i<s->count;i++){
  hone_graph_fragment f;int status=hone_graph_fragment_at(&s->packets[i].triangle,x,y,depth,s->drivers,s->driver_count,scratch,scratch_count,&f);
  if(status<0)return -1;if(!status)continue;
  uint32_t order=s->packets[i].order;
  if(opaque!=SIZE_MAX&&(f.z<opaque_z||(f.z==opaque_z&&
      (order>opaque_order||(order==opaque_order&&i>opaque)))))continue;
  if(n==4)return -1;
  hone_graph_ordered item={f,order};unsigned at=n++;
  while(at&&(staged[at-1].fragment.z>f.z||(staged[at-1].fragment.z==f.z&&staged[at-1].order<=item.order))){staged[at]=staged[at-1];at--;}
  staged[at]=item;
 }
 memcpy(out,staged,n*sizeof *out);return (int)n;
}
#endif
