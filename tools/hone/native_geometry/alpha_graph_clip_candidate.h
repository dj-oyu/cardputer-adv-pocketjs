#ifndef HONE_ALPHA_GRAPH_CLIP_CANDIDATE_H
#define HONE_ALPHA_GRAPH_CLIP_CANDIDATE_H
#include "alpha_graph_fragment_candidate.h"
/* Host-only orthographic viewport clip; coordinates, depth and deformed local
 * attributes use the same linear edge parameter. Programs are borrowed from
 * the immutable input. This does not perform camera/near clipping, shading,
 * frame publication or DMA ownership. Output commits only after full success.
 * Caller owns nonoverlapping input, output and output-count storage. */
static inline int hone_graph_clip_viewport(const hone_graph_triangle *input,
 float left,float top,float right,float bottom,hone_graph_triangle *out,size_t capacity,size_t *count){
 if(!input||!out||!count||!input->alpha||!input->color||!isfinite(left)||!isfinite(top)||
    !isfinite(right)||!isfinite(bottom)||!(left<right)||!(top<bottom))return 0;
 int inside=1;
 for(unsigned i=0;i<3;i++){
  const hone_graph_vertex *v=&input->v[i];
  if(!isfinite(v->x)||!isfinite(v->y)||!isfinite(v->z))return 0;
  for(unsigned k=0;k<3;k++)if(!isfinite(v->local[k]))return 0;
  if(v->x<left||v->x>right||v->y<top||v->y>bottom)inside=0;
 }
 if(inside){
  if(!capacity)return 0;
  *out=*input;
  *count=1;
  return 1;
 }
 hone_graph_vertex a[8],b[8];memcpy(a,input->v,sizeof input->v);size_t n=3;
 const double edge[4]={left,right,top,bottom};
 for(unsigned side=0;side<4;side++){
  size_t used=0;if(!n)break;
  for(size_t i=0;i<n;i++){
   const hone_graph_vertex *p=&a[i],*q=&a[(i+1)%n];
   double pv=side<2?p->x:p->y,qv=side<2?q->x:q->y;
   int pin=(side%2==0)?pv>=edge[side]:pv<=edge[side];
   int qin=(side%2==0)?qv>=edge[side]:qv<=edge[side];
   if(pin){if(used>=8)return 0;b[used++]=*p;}
   if(pin!=qin){
    double t=(edge[side]-pv)/(qv-pv);if(!isfinite(t)||t<0||t>1||used>=8)return 0;
    hone_graph_vertex v;
    double xyz[6]={(double)p->x+t*((double)q->x-p->x),(double)p->y+t*((double)q->y-p->y),
      (double)p->z+t*((double)q->z-p->z),(double)p->local[0]+t*((double)q->local[0]-p->local[0]),
      (double)p->local[1]+t*((double)q->local[1]-p->local[1]),(double)p->local[2]+t*((double)q->local[2]-p->local[2])};
    for(unsigned k=0;k<6;k++)if(!isfinite(xyz[k])||fabs(xyz[k])>FLT_MAX)return 0;
    v.x=(float)xyz[0];v.y=(float)xyz[1];v.z=(float)xyz[2];
    for(unsigned k=0;k<3;k++)v.local[k]=(float)xyz[3+k];
    if(side<2)v.x=(float)edge[side];else v.y=(float)edge[side];
    b[used++]=v;
   }
  }
  n=used;memcpy(a,b,n*sizeof *a);
 }
 size_t triangles=n>=3?n-2:0;if(triangles>capacity)return 0;
 hone_graph_triangle staged[6];
 for(size_t i=0;i<triangles;i++){
  staged[i]=(hone_graph_triangle){{a[0],a[i+1],a[i+2]},input->alpha,input->color};
 }
 memcpy(out,staged,triangles*sizeof *out);*count=triangles;return 1;
}
#endif
