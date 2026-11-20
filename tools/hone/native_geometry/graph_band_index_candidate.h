#ifndef HONE_GRAPH_BAND_INDEX_CANDIDATE_H
#define HONE_GRAPH_BAND_INDEX_CANDIDATE_H
#include "alpha_graph_occlusion_band_candidate.h"
typedef struct {uint16_t x0,x1,y0,y1;} hone_graph_bounds;
typedef struct {
 hone_graph_snapshot graph;const hone_graph_snapshot*source;uint64_t revision;
 hone_graph_bounds*bounds;size_t bounds_capacity;uint16_t*ids;size_t ids_capacity,used;
 size_t offsets[18];int ready;
} hone_graph_band_index;
/* Conservative integer sample bounds; validate even fully off-screen geometry.
 * Input/program/driver/index dependencies are immutable through preflight/replay.
 * Revision is supplied by the exclusive frame owner, never reused for new data. */
static inline int hone_graph_index_bounds(const hone_graph_triangle*t,hone_graph_bounds*out){
 if(!t||!t->alpha||!t->color)return 0;double xmin=INFINITY,xmax=-INFINITY,ymin=INFINITY,ymax=-INFINITY;
 for(unsigned k=0;k<3;k++){
  const hone_graph_vertex*v=&t->v[k];if(!isfinite(v->x)||!isfinite(v->y)||!isfinite(v->z))return 0;
  for(unsigned j=0;j<3;j++)if(!isfinite(v->local[j]))return 0;
  if(v->x<xmin)xmin=v->x;if(v->x>xmax)xmax=v->x;if(v->y<ymin)ymin=v->y;if(v->y>ymax)ymax=v->y;
 }
 if(!isfinite(hone_graph_edge(t->v[0],t->v[1],t->v[2].x,t->v[2].y)))return 0;
 if(xmax<0||xmin>239||ymax<0||ymin>134){*out=(hone_graph_bounds){1,0,1,0};return 1;}
 *out=(hone_graph_bounds){(uint16_t)ceil(fmax(0,xmin)),(uint16_t)floor(fmin(239,xmax)),(uint16_t)ceil(fmax(0,ymin)),(uint16_t)floor(fmin(134,ymax))};return 1;
}
static inline int hone_graph_index_build(hone_graph_band_index*out,const hone_graph_snapshot*s,uint64_t revision,
 hone_graph_bounds*bounds,size_t capacity,uint16_t*ids,size_t ids_capacity){
 if(!out||!s||!revision||!s->ready||!s->owner||!s->generation||s->count>s->capacity||s->count>65535||
    (s->count&&(!s->packets||!bounds))||s->count>capacity||capacity>65535||ids_capacity>17*65535u||(ids_capacity&&!ids))return 0;
 const void*w[]={out,bounds,ids};size_t z[]={sizeof *out,capacity*sizeof *bounds,ids_capacity*sizeof *ids};
 for(unsigned j=0;j<3;j++)if(z[j]){
  if(!hone_alpha_disjoint(w[j],z[j],s,sizeof *s)||!hone_alpha_disjoint(w[j],z[j],s->packets,s->count*sizeof *s->packets)||
     (s->driver_count&&(!s->drivers||s->driver_count>65536||!hone_alpha_disjoint(w[j],z[j],s->drivers,s->driver_count*sizeof *s->drivers))))return 0;
  for(unsigned k=0;k<j;k++)if(z[k]&&!hone_alpha_disjoint(w[j],z[j],w[k],z[k]))return 0;
  for(size_t k=0;k<s->count;k++){
   const hone_graph_triangle*t=&s->packets[k].triangle;
   if(!t->alpha||!t->color||!hone_alpha_disjoint(w[j],z[j],t->alpha,sizeof *t->alpha)||!hone_alpha_disjoint(w[j],z[j],t->color,3*sizeof *t->color))return 0;
   for(unsigned n=0;n<4;n++){const hone_alpha_program*p=n?t->color+n-1:t->alpha;if(!p->code||!p->count||p->count>1024||p->result>=p->count||!hone_alpha_disjoint(w[j],z[j],p->code,p->count*sizeof *p->code))return 0;}
  }
 }
 size_t counts[17]={0},total=0;
 for(size_t i=0;i<s->count;i++){hone_graph_bounds b;if(!hone_graph_index_bounds(&s->packets[i].triangle,&b))return 0;if(b.x0>b.x1||b.y0>b.y1)continue;for(unsigned y=b.y0/8;y<=b.y1/8;y++){counts[y]++;total++;}}
 if(total>ids_capacity)return 0;
 hone_graph_band_index result={.graph=*s,.source=s,.revision=revision,.bounds=bounds,.bounds_capacity=capacity,.ids=ids,.ids_capacity=ids_capacity,.used=total,.ready=1};
 for(unsigned y=0;y<17;y++)result.offsets[y+1]=result.offsets[y]+counts[y];size_t cursor[17];memcpy(cursor,result.offsets,sizeof cursor);
 for(size_t i=0;i<s->count;i++){hone_graph_bounds b;hone_graph_index_bounds(&s->packets[i].triangle,&b);bounds[i]=b;if(b.x0>b.x1||b.y0>b.y1)continue;for(unsigned y=b.y0/8;y<=b.y1/8;y++)ids[cursor[y]++]=(uint16_t)i;}
 *out=result;return 1;
}
static inline int hone_graph_index_matches(const hone_graph_band_index*i,const hone_graph_snapshot*s,uint64_t revision){
 return i&&s&&i->ready&&i->source==s&&i->revision==revision&&revision&&s->ready&&i->graph.packets==s->packets&&i->graph.count==s->count&&i->graph.capacity==s->capacity&&i->graph.drivers==s->drivers&&i->graph.driver_count==s->driver_count&&i->graph.owner==s->owner&&i->graph.generation==s->generation;
}
static inline int hone_graph_index_fragments(const hone_graph_band_index*i,const hone_graph_snapshot*s,uint64_t revision,unsigned x,unsigned y,float depth,float*scratch,size_t scratch_count,hone_graph_ordered out[4]){
 if(!hone_graph_index_matches(i,s,revision)||x>=240||y>=135||!out||!scratch||!scratch_count||scratch_count>1024||isnan(depth)||depth==INFINITY)return -1;
 size_t begin=i->offsets[y/8],end=i->offsets[y/8+1],opaque=SIZE_MAX;float z=0;uint32_t order=0;hone_graph_ordered kept[4];unsigned n=0;
 if(end<begin||end>i->used||i->used>i->ids_capacity)return -1;
 for(unsigned pass=0;pass<2;pass++)for(size_t at=begin;at<end;at++){
  size_t k=i->ids[at];if(k>=s->count)return -1;hone_graph_bounds b=i->bounds[k];if(x<b.x0||x>b.x1||y<b.y0||y>b.y1)continue;
  hone_graph_fragment f;int status=hone_graph_fragment_at(&s->packets[k].triangle,x,y,depth,s->drivers,s->driver_count,scratch,scratch_count,&f);if(status<0)return -1;if(!status)continue;uint32_t key=s->packets[k].order;
  if(!pass){if(f.alpha==1&&(opaque==SIZE_MAX||f.z>z||(f.z==z&&key<order))){opaque=k;z=f.z;order=key;}continue;}
  if(opaque!=SIZE_MAX&&(f.z<z||(f.z==z&&(key>order||(key==order&&k>opaque)))))continue;
  if(n==4)return -1;hone_graph_ordered item={f,key};unsigned pos=n++;
  while(pos&&(kept[pos-1].fragment.z>f.z||(kept[pos-1].fragment.z==f.z&&kept[pos-1].order<=key))){kept[pos]=kept[pos-1];pos--;}kept[pos]=item;
 }
 memcpy(out,kept,n*sizeof *out);return (int)n;
}
#endif
