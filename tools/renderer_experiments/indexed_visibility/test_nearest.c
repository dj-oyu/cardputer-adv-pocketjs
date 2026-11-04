#include "visibility.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
static vis_generation g={1,1,1};
static vis_limits limits={2048,4095,524288};
static vis_budget ample(void){vis_budget b={{100000,10000000,10000000},{0,0,0}};return b;}
static uint32_t rng=1234567;
static double random01(void){rng=rng*1664525u+1013904223u;return (double)(rng>>8)/16777216.;}
static void cross(const double a[3],const double b[3],double c[3]){c[0]=a[1]*b[2]-a[2]*b[1];c[1]=a[2]*b[0]-a[0]*b[2];c[2]=a[0]*b[1]-a[1]*b[0];}
static double dot(const double a[3],const double b[3]){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
/* Independent Moller-Trumbore scan, no BVH/projection implementation reuse. */
static int brute(const double *v,size_t n,const double o[3],const double d[3],double *best,uint32_t *ordinal){
 int found=0;*best=INFINITY;
 for(size_t i=0;i<n;i++){const double *p=v+9*i;double e1[3],e2[3],q[3],s[3],r[3];
  for(int k=0;k<3;k++){e1[k]=p[3+k]-p[k];e2[k]=p[6+k]-p[k];s[k]=o[k]-p[k];}
  cross(d,e2,q);double det=dot(e1,q);if(fabs(det)<1e-14)continue;
  double u=dot(s,q)/det;if(u<0 || u>1)continue;cross(s,e1,r);
  double w=dot(d,r)/det;if(w<0 || u+w>1)continue;
  double t=dot(e2,r)/det;if(t<=VIS_SEGMENT_EPSILON || t>=*best)continue;
  *best=t;*ordinal=(uint32_t)i;found=1;
 }return found;
}
int main(void){
 double v[]={-1,-1,2,1,-1,2,0,1,2, -1,-1,1,1,-1,1,0,1,1, -1,-1,1,1,-1,1,0,1,1};
 uint32_t indices[]={0,1,2,3,4,5,6,7,8};vis_scene *s=NULL;vis_binding b={0};
 assert(vis_scene_build(&s,v,9,indices,3,g,limits)==VIS_OK);assert(vis_binding_set(&b,s,g)==VIS_OK);
 double o[]={0,0,0},d[]={0,0,1};vis_budget budget=ample();vis_hit h;
 assert(vis_ray_nearest(&b,g,o,d,&budget,&h)==VIS_OK && h.found && h.triangle_ordinal==1 && h.distance==1);
 assert(fabs(h.bary[0]-.25)<1e-12 && fabs(h.bary[1]-.25)<1e-12 && fabs(h.bary[2]-.5)<1e-12);
 assert(h.position[2]==1 && h.geometric_normal[2]==1);
 double edge[]={0,-1,0};assert(vis_ray_nearest(&b,g,edge,d,&budget,&h)==VIS_OK && h.found && h.distance==1);
 double sky[]={0,0,-1};assert(vis_ray_nearest(&b,g,o,sky,&budget,&h)==VIS_OK && !h.found && h.distance==0);
 double near[]={0,0,1-0.5e-5};assert(vis_ray_nearest(&b,g,near,d,&budget,&h)==VIS_OK && h.found && h.triangle_ordinal==0);
 vis_hit sentinel;memset(&sentinel,0x5a,sizeof(sentinel));h=sentinel;
 double zero[]={0,0,0},nan[]={NAN,0,1},nonunit[]={0,0,2};
 assert(vis_ray_nearest(&b,g,o,zero,&budget,&h)==VIS_INVALID && !memcmp(&h,&sentinel,sizeof(h)));
 assert(vis_ray_nearest(&b,g,o,nan,&budget,&h)==VIS_INVALID && !memcmp(&h,&sentinel,sizeof(h)));
 assert(vis_ray_nearest(&b,g,nan,d,&budget,&h)==VIS_INVALID && !memcmp(&h,&sentinel,sizeof(h)));
 assert(vis_ray_nearest(&b,g,o,nonunit,&budget,&h)==VIS_INVALID && !memcmp(&h,&sentinel,sizeof(h)));
 vis_generation wrong={1,2,1};assert(vis_ray_nearest(&b,wrong,o,d,&budget,&h)==VIS_GENERATION && !memcmp(&h,&sentinel,sizeof(h)));
 for(int k=0;k<3;k++){vis_budget exhausted=ample();h=sentinel;
  if(k==0)exhausted.limit.rays=0;else if(k==1)exhausted.limit.node_visits=0;else exhausted.limit.triangle_tests=1;
  assert(vis_ray_nearest(&b,g,o,d,&exhausted,&h)==VIS_BUDGET && !memcmp(&h,&sentinel,sizeof(h)));
 }
 memset(v,0xff,sizeof(v));memset(indices,0xff,sizeof(indices));vis_scene_release(s);s=NULL;
 assert(vis_ray_nearest(&b,g,o,d,&budget,&h)==VIS_OK && h.found && h.distance==1);vis_binding_clear(&b);
 /* Negative-coordinate reflected winding and distant geometry, no fixed clip. */
 double farv[]={-9,-11,-1e8,-11,-11,-1e8,-10,-9,-1e8};uint32_t fi[]={0,1,2};
 assert(vis_scene_build(&s,farv,3,fi,1,g,limits)==VIS_OK);assert(vis_binding_set(&b,s,g)==VIS_OK);
 double faro[]={-10,-10,0};assert(vis_ray_nearest(&b,g,faro,sky,&budget,&h)==VIS_OK && h.found && h.distance==1e8 && h.geometric_normal[2]==-1);
 vis_binding_clear(&b);vis_scene_release(s);s=NULL;
 enum{N=64};double randomv[N*9];uint32_t ri[N*3];
 for(size_t i=0;i<N*9;i++)randomv[i]=random01()*20-10;
 for(size_t i=0;i<N*3;i++)ri[i]=(uint32_t)i;
 assert(vis_scene_build(&s,randomv,N*3,ri,N,g,limits)==VIS_OK);assert(vis_binding_set(&b,s,g)==VIS_OK);
 budget=ample();size_t hitcount=0,misscount=0;
 for(size_t i=0;i<1000;i++){double origin[3],direction[3];
  for(int k=0;k<3;k++){origin[k]=random01()*40-20;direction[k]=random01()*2-1;}
  if(i%2==0){size_t target=i%N;for(int k=0;k<3;k++)direction[k]=(randomv[9*target+k]+randomv[9*target+3+k]+randomv[9*target+6+k])/3-origin[k];}
  double l=hypot(hypot(direction[0],direction[1]),direction[2]);for(int k=0;k<3;k++)direction[k]/=l;
  double distance;uint32_t ordinal=0;int found=brute(randomv,N,origin,direction,&distance,&ordinal);
  assert(vis_ray_nearest(&b,g,origin,direction,&budget,&h)==VIS_OK && h.found==found);
  if(found){hitcount++;assert(h.triangle_ordinal==ordinal && fabs(h.distance-distance)<1e-9*fmax(1,distance));
   assert(fabs(h.bary[0]+h.bary[1]+h.bary[2]-1)<1e-12);
   for(int k=0;k<3;k++){double reconstructed=0;for(int j=0;j<3;j++)reconstructed+=h.bary[j]*randomv[9*ordinal+3*j+k];assert(fabs(reconstructed-h.position[k])<1e-9);}
  }else misscount++;
 }
 assert(hitcount && misscount);vis_binding_clear(&b);vis_scene_release(s);
 printf("PASS nearest known/miss/edge/epsilon/tie/negative/far/invalid/budget/poison/lifetime; brute-force 1000 rays: %zu hits, %zu misses\n",hitcount,misscount);
 return 0;
}
