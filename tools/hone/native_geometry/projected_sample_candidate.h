#ifndef HONE_PROJECTED_SAMPLE_CANDIDATE_H
#define HONE_PROJECTED_SAMPLE_CANDIDATE_H
#include "projected_snapshot_candidate.h"
/* Minimal no-expanded-array reference consumer. It intentionally scans packets;
 * specialized raster/index kernels can use the same at/color accessors instead. */
static inline double hone_projected_edge(const hone_projected_packet*p,unsigned a,unsigned b,double x,double y){
 return ((double)p->xyz[b*3]-p->xyz[a*3])*(y-p->xyz[a*3+1])-((double)p->xyz[b*3+1]-p->xyz[a*3+1])*(x-p->xyz[a*3]);
}
static inline int hone_projected_sample(const hone_projected_snapshot*s,size_t index,unsigned x,unsigned y,float opaque_depth,hone_graph_fragment*out){
 const hone_projected_packet*p=hone_projected_at(s,index);const hone_projected_material*m=hone_projected_color(s,p);
 if(!p||!m||!out||isnan(opaque_depth)||opaque_depth==INFINITY)return -1;
 for(unsigned k=0;k<9;k++)if(!isfinite(p->xyz[k]))return -1;
 for(unsigned k=0;k<3;k++)if(!isfinite(m->rgb[k])||m->rgb[k]<0||m->rgb[k]>1)return -1;
 unsigned v[3]={0,1,2};double area=hone_projected_edge(p,0,1,p->xyz[6],p->xyz[7]);
 if(!isfinite(area))return -1;if(!area)return 0;if(area<0){v[1]=2;v[2]=1;area=-area;}
 double weights[3];
 for(unsigned j=0;j<3;j++){
  unsigned a=v[(j+1)%3],b=v[(j+2)%3];double e=hone_projected_edge(p,a,b,x,y);
  if(!isfinite(e))return -1;
  if(e<0||(e==0&&!(p->xyz[b*3+1]<p->xyz[a*3+1]||(p->xyz[b*3+1]==p->xyz[a*3+1]&&p->xyz[b*3]>p->xyz[a*3]))))return 0;
  weights[j]=e/area;
 }
 double depth=0;for(unsigned j=0;j<3;j++)depth+=weights[j]*p->xyz[v[j]*3+2];
 if(!isfinite(depth)||fabs(depth)>FLT_MAX)return -1;if(depth<=opaque_depth)return 0;
 hone_graph_fragment f={.z=(float)depth,.alpha=1,.rgb={m->rgb[0],m->rgb[1],m->rgb[2]}};*out=f;return 1;
}
static inline int hone_projected_pixel(const hone_projected_snapshot*s,unsigned x,unsigned y,float opaque_depth,uint16_t*out){
 if(!s||!s->ready||s->failed||s->count>s->capacity||!out||x>=240||y>=135)return -1;
 hone_graph_fragment best={0};uint32_t order=0;bool hit=false;
 for(size_t i=0;i<s->count;i++){
  hone_graph_fragment f;int status=hone_projected_sample(s,i,x,y,opaque_depth,&f);if(status<0)return -1;if(!status)continue;
  if(!hit||f.z>best.z||(f.z==best.z&&s->packets[i].order<order)){best=f;order=s->packets[i].order;hit=true;}
 }
 if(!hit)return 0;
 *out=(uint16_t)(((unsigned)floorf(best.rgb[0]*31+.5f)<<11)|((unsigned)floorf(best.rgb[1]*63+.5f)<<5)|(unsigned)floorf(best.rgb[2]*31+.5f));return 1;
}
#endif
