#ifndef HONE_ALPHA_GRAPH_FRAGMENT_CANDIDATE_H
#define HONE_ALPHA_GRAPH_FRAGMENT_CANDIDATE_H
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "base_color_candidate.h"
/* Diagnostic fragment bridge for orthographic, already-clipped triangles.
 * Each vertex carries the DEFORMED source-object-local coordinate, not a
 * per-vertex alpha. Position, depth, attributes, programs and drivers must
 * belong to one immutable frame; scratch/output are caller-owned and disjoint.
 * Returns 1=fragment, 0=no coverage, -1=invalid/evaluation failure. No write
 * to out on 0/-1. RGB is unlit base color; this is not a shading or LCD path. */
typedef struct {float x,y,z,local[3];} hone_graph_vertex;
typedef struct {hone_graph_vertex v[3];const hone_alpha_program *alpha,*color;} hone_graph_triangle;
typedef struct {float z,alpha,rgb[3];} hone_graph_fragment;
static inline double hone_graph_edge(hone_graph_vertex a,hone_graph_vertex b,double x,double y){
 return ((double)b.x-a.x)*(y-a.y)-((double)b.y-a.y)*(x-a.x);
}
static inline int hone_graph_fragment_at(const hone_graph_triangle *triangle,unsigned x,unsigned y,float opaque_depth,
 const double *drivers,size_t driver_count,float *scratch,size_t capacity,hone_graph_fragment *out){
 if(!triangle||!triangle->alpha||!triangle->color||!scratch||!out||isnan(opaque_depth)||opaque_depth==INFINITY)return -1;
 hone_graph_vertex v[3];memcpy(v,triangle->v,sizeof v);
 for(unsigned i=0;i<3;i++){
  if(!isfinite(v[i].x)||!isfinite(v[i].y)||!isfinite(v[i].z))return -1;
  for(unsigned k=0;k<3;k++)if(!isfinite(v[i].local[k]))return -1;
 }
 double area=hone_graph_edge(v[0],v[1],v[2].x,v[2].y);
 if(!isfinite(area))return -1;if(area==0)return 0;
 if(area<0){hone_graph_vertex tmp=v[1];v[1]=v[2];v[2]=tmp;area=-area;}
 double weights[3];
 for(unsigned i=0;i<3;i++){
  hone_graph_vertex a=v[(i+1)%3],b=v[(i+2)%3];double e=hone_graph_edge(a,b,x,y);
  if(!isfinite(e))return -1;
  if(e<0||(e==0&&!(b.y<a.y||(b.y==a.y&&b.x>a.x))))return 0;
  weights[i]=e/area;
 }
 double depth=0,local[3]={0,0,0};
 for(unsigned i=0;i<3;i++){
  depth+=weights[i]*v[i].z;
  for(unsigned k=0;k<3;k++)local[k]+=weights[i]*v[i].local[k];
 }
 if(!isfinite(depth)||fabs(depth)>FLT_MAX)return -1;
 if(depth<=opaque_depth)return 0;
 float xyz[3];for(unsigned k=0;k<3;k++){if(!isfinite(local[k])||fabs(local[k])>FLT_MAX)return -1;xyz[k]=(float)local[k];}
 hone_graph_fragment fragment;fragment.z=(float)depth;
 if(!hone_alpha_factor(triangle->alpha,xyz,drivers,driver_count,scratch,capacity,&fragment.alpha))return -1;
 if(fragment.alpha==0)return 0;
 if(!hone_base_color(triangle->color,xyz,drivers,driver_count,scratch,capacity,fragment.rgb))return -1;
 *out=fragment;return 1;
}
#endif
