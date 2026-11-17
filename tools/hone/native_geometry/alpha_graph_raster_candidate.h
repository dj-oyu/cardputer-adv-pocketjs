#ifndef HONE_ALPHA_GRAPH_RASTER_CANDIDATE_H
#define HONE_ALPHA_GRAPH_RASTER_CANDIDATE_H
#include "alpha_graph_fragment_candidate.h"
#include "../source_package/alpha_packet_candidate.h"
/* Host-only preclipped orthographic alpha lane. RGB is unlit base color.
 * Immutable snapshot, programs, driver inputs, depth and scratch must remain
 * alive and pairwise disjoint from pixels for preflight and replay. This does
 * not build/clip packets, preserve source attributes through clipping, apply
 * Principled lighting, coordinate with LCD DMA, or admit the target budget. */
typedef struct {hone_graph_triangle triangle;uint32_t order;} hone_graph_packet;
typedef struct {
 const hone_graph_packet *packets;size_t count,capacity;
 const double *drivers;size_t driver_count;const void *owner;uint32_t generation;int ready;
} hone_graph_snapshot;
typedef struct {hone_graph_fragment fragment;uint32_t order;} hone_graph_ordered;
static inline int hone_graph_fragments(const hone_graph_snapshot *s,unsigned x,unsigned y,float depth,
 float *scratch,size_t scratch_count,hone_graph_ordered out[4]){
 unsigned n=0;
 for(size_t i=0;i<s->count;i++){
  hone_graph_fragment f;int status=hone_graph_fragment_at(&s->packets[i].triangle,x,y,depth,s->drivers,s->driver_count,scratch,scratch_count,&f);
  if(status<0)return -1;if(!status)continue;if(n==4)return -1;
  hone_graph_ordered item={f,s->packets[i].order};unsigned at=n++;
  while(at&&(out[at-1].fragment.z>f.z||(out[at-1].fragment.z==f.z&&out[at-1].order<=item.order))){out[at]=out[at-1];at--;}
  out[at]=item;
 }
 return (int)n;
}
static inline int hone_graph_render(const hone_graph_snapshot *s,const void *owner,uint32_t generation,
 uint16_t *pixels,const float *depth,unsigned width,unsigned height,unsigned rows,unsigned span_limit,
 float *scratch,size_t scratch_count){
 if(!s||!owner||s->owner!=owner||s->generation!=generation||!generation||!s->ready||s->count>s->capacity)return 0;
 if(!s->count)return 1;
 if(!s->packets||!pixels||!depth||!scratch||!scratch_count||scratch_count>1024||!width||width>240||!height||height>135||
    (rows!=1&&rows!=4&&rows!=8)||!span_limit||span_limit>width||s->count>65536)return 0;
 size_t size=(size_t)width*height;
 if(!hone_alpha_disjoint(pixels,size*sizeof *pixels,depth,size*sizeof *depth)||
    !hone_alpha_disjoint(pixels,size*sizeof *pixels,s->packets,s->count*sizeof *s->packets)||
    !hone_alpha_disjoint(pixels,size*sizeof *pixels,s,sizeof *s)||
    !hone_alpha_disjoint(pixels,size*sizeof *pixels,scratch,scratch_count*sizeof *scratch)||
    !hone_alpha_disjoint(scratch,scratch_count*sizeof *scratch,depth,size*sizeof *depth)||
    !hone_alpha_disjoint(scratch,scratch_count*sizeof *scratch,s->packets,s->count*sizeof *s->packets)||
    !hone_alpha_disjoint(scratch,scratch_count*sizeof *scratch,s,sizeof *s)||
    (s->driver_count&&(!s->drivers||s->driver_count>65536||!hone_alpha_disjoint(pixels,size*sizeof *pixels,s->drivers,s->driver_count*sizeof *s->drivers))))return 0;
 if(s->driver_count&&!hone_alpha_disjoint(scratch,scratch_count*sizeof *scratch,s->drivers,s->driver_count*sizeof *s->drivers))return 0;
 for(size_t i=0;i<s->count;i++){
  const hone_alpha_program *a=s->packets[i].triangle.alpha,*c=s->packets[i].triangle.color;
  if(!a||!c||!hone_alpha_disjoint(scratch,scratch_count*sizeof *scratch,a,sizeof *a)||
     !hone_alpha_disjoint(scratch,scratch_count*sizeof *scratch,c,3*sizeof *c)||
     !hone_alpha_disjoint(pixels,size*sizeof *pixels,a,sizeof *a)||!hone_alpha_disjoint(pixels,size*sizeof *pixels,c,3*sizeof *c))return 0;
  for(unsigned k=0;k<4;k++){
   const hone_alpha_program *p=k?&c[k-1]:a;
   if(!p->code||!p->count||p->count>1024||
      !hone_alpha_disjoint(scratch,scratch_count*sizeof *scratch,p->code,p->count*sizeof *p->code)||
      !hone_alpha_disjoint(pixels,size*sizeof *pixels,p->code,p->count*sizeof *p->code))return 0;
  }
 }
 /* Full preflight prevents malformed late pixels or slot/span overflow from
  * touching the destination. Inputs must remain immutable during replay. */
 for(unsigned y=0;y<height;y++){unsigned left=width,right=0;for(unsigned x=0;x<width;x++){
  float z=depth[(size_t)y*width+x];if(isnan(z)||z==INFINITY)return 0;
  hone_graph_ordered f[4];int n=hone_graph_fragments(s,x,y,z,scratch,scratch_count,f);if(n<0)return 0;
  if(n){if(x<left)left=x;right=x+1;}
 }if(right&&right-left>span_limit)return 0;}
 for(unsigned band=0;band<height;band+=rows)for(unsigned y=band;y<height&&y<band+rows;y++)for(unsigned x=0;x<width;x++){
  size_t at=(size_t)y*width+x;hone_graph_ordered f[4];int n=hone_graph_fragments(s,x,y,depth[at],scratch,scratch_count,f);
  if(n<=0)continue;
  float rgb[3]={(pixels[at]>>11)/31.f,((pixels[at]>>5)&63)/63.f,(pixels[at]&31)/31.f};
  for(int i=0;i<n;i++)for(unsigned k=0;k<3;k++)rgb[k]=f[i].fragment.rgb[k]*f[i].fragment.alpha+rgb[k]*(1-f[i].fragment.alpha);
  unsigned r=(unsigned)floorf(rgb[0]*31+.5f),g=(unsigned)floorf(rgb[1]*63+.5f),b=(unsigned)floorf(rgb[2]*31+.5f);
  pixels[at]=(uint16_t)((r<<11)|(g<<5)|b);
 }
 return 1;
}
#endif
