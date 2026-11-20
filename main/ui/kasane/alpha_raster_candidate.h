#ifndef HONE_ALPHA_RASTER_CANDIDATE_H
#define HONE_ALPHA_RASTER_CANDIDATE_H
#include "alpha_packet_candidate.h"
/* Native host integration lane. Full immutable opaque depth is supplied by
 * caller here, NOT a claim of production low-memory depth availability. */
typedef struct {float z,alpha,rgb[3];
 uint32_t order;
 } hone_alpha_fragment;

static inline double hone_alpha_edge(hone_alpha_vertex a,hone_alpha_vertex b,double x,double y){return ((double)b.x-a.x)*(y-a.y)-((double)b.y-a.y)*(x-a.x);
 }
static inline int hone_alpha_sample(const hone_alpha_packet*p,unsigned x,unsigned y,float opaque,hone_alpha_fragment*out){
 hone_alpha_vertex v[3];
 memcpy(v,p->v,sizeof v);
 double area=hone_alpha_edge(v[0],v[1],v[2].x,v[2].y);

 if(area==0)return 0;

 if(area<0){hone_alpha_vertex t=v[1];
 v[1]=v[2];
 v[2]=t;
 area=-area;
 }
 double w[3];
 for(unsigned i=0;i<3;i++){hone_alpha_vertex a=v[(i+1)%3],b=v[(i+2)%3];
 double e=hone_alpha_edge(a,b,x,y);

 if(e<0||(e==0&&!(b.y<a.y||(b.y==a.y&&b.x>a.x))))return 0;
 w[i]=e/area;
 }
 double z=0,mask=0;
 for(unsigned i=0;i<3;i++){z+=w[i]*v[i].z;
 mask+=w[i]*v[i].mask;
 }float alpha=p->mask_enabled&&mask>p->mask_low&&mask<p->mask_high?1:p->alpha;

 if(z<=opaque||alpha==0)return 0;

 *out=(hone_alpha_fragment){(float)z,alpha,{p->rgb[0],p->rgb[1],p->rgb[2]},p->order};
 return 1;

}
static inline int hone_alpha_fragments(const hone_alpha_snapshot*s,unsigned x,unsigned y,float opaque,hone_alpha_fragment f[4]){
 unsigned n=0;
 for(size_t i=0;i<s->count;i++){hone_alpha_fragment q;

 if(!hone_alpha_sample(s->packets+i,x,y,opaque,&q))continue;

 if(n==4)return -1;
 unsigned at=n;
 while(at&&(f[at-1].z>q.z||(f[at-1].z==q.z&&f[at-1].order<=q.order))){f[at]=f[at-1];
 at--;
 }f[at]=q;
 n++;
 }return (int)n;

}
/* Zero packets are an immediate no-op: no opaque pixel/depth scan. Nonempty
 * lane preflights all pixels before output mutation, then immutable replay.
 * Owner holds ready packets, pixels and depth exclusively for both passes.
 * No LCD writes occur here. Failure leaves the entire pixel destination intact. */
static inline int hone_alpha_render(const hone_alpha_snapshot*s,const void*owner,uint32_t generation,uint16_t*pixels,const float*depth,unsigned width,unsigned height,unsigned rows,unsigned span_limit){
 if(!s||!owner||s->owner!=owner||s->generation!=generation||!generation||!s->ready||s->count>s->capacity)return 0;

 if(!s->count)return 1;

 if(!s->packets||!pixels||!depth||!width||width>240||!height||height>135||(rows!=1&&rows!=4&&rows!=8)||!span_limit||span_limit>width||s->count>65536)return 0;

 size_t size=(size_t)width*height;

 if(!hone_alpha_disjoint(pixels,size*sizeof *pixels,depth,size*sizeof *depth)||!hone_alpha_disjoint(pixels,size*sizeof *pixels,s->packets,s->count*sizeof *s->packets)||!hone_alpha_disjoint(pixels,size*sizeof *pixels,s,sizeof *s))return 0;

 for(size_t i=0;i<s->count;i++){hone_alpha_packet temp[3];

 if(hone_alpha_clip(s->packets+i,-1e20f,1e20f,temp)<0)return 0;
 for(unsigned j=0;j<3;j++)if(fabsf(s->packets[i].v[j].x)>1e6f||fabsf(s->packets[i].v[j].y)>1e6f||fabsf(s->packets[i].v[j].z)>1e10f||fabsf(s->packets[i].v[j].mask)>1e10f)return 0;
 }
 for(unsigned y=0;y<height;y++){unsigned left=width,right=0;
 for(unsigned x=0;x<width;x++){
  if(isnan(depth[(size_t)y*width+x])||depth[(size_t)y*width+x]==INFINITY)return 0;

 hone_alpha_fragment f[4];
 int n=hone_alpha_fragments(s,x,y,depth[(size_t)y*width+x],f);

 if(n<0)return 0;

 if(n){if(x<left)left=x;
 right=x+1;
 }}
  if(right&&right-left>span_limit)return 0;

 }
 for(unsigned band=0;band<height;band+=rows)for(unsigned y=band;y<height&&y<band+rows;y++)for(unsigned x=0;x<width;x++){
  size_t at=(size_t)y*width+x;

 hone_alpha_fragment f[4];
 int n=hone_alpha_fragments(s,x,y,depth[at],f);

 if(!n)continue;

  float rgb[3]={(pixels[at]>>11)/31.f,((pixels[at]>>5)&63)/63.f,(pixels[at]&31)/31.f};

  for(int i=0;i<n;i++)for(unsigned k=0;k<3;k++)rgb[k]=f[i].rgb[k]*f[i].alpha+rgb[k]*(1-f[i].alpha);

  unsigned r=(unsigned)floorf(rgb[0]*31+.5f),g=(unsigned)floorf(rgb[1]*63+.5f),b=(unsigned)floorf(rgb[2]*31+.5f);
 pixels[at]=(uint16_t)((r<<11)|(g<<5)|b);

 }
 return 1;

}
#endif
