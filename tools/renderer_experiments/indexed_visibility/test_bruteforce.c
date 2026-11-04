/* Independent public-API BVH oracle: no production source inclusion. */
#include "visibility.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

#define TRIANGLES 64u
#define SEGMENTS 1000u
static uint32_t seed=UINT32_C(0x6f782026);
static double random_signed(void) {
 seed=seed*UINT32_C(1664525)+UINT32_C(1013904223);
 return 2.0*(double)(seed>>8)/16777216.0-1.0;
}
static void transform(const double p[3],double q[3]) {
 const double c=0.8,s=0.6;
 q[0]=-2+c*(1.7*p[0])+s*(0.6*p[2]);
 q[1]=1+0.9*p[1];
 q[2]=-4-s*(1.7*p[0])+c*(0.6*p[2]);
}
static long double dot(const long double a[3],const long double b[3]) {
 return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];
}
static void cross(const long double a[3],const long double b[3],long double o[3]) {
 o[0]=a[1]*b[2]-a[2]*b[1];
 o[1]=a[2]*b[0]-a[0]*b[2];
 o[2]=a[0]*b[1]-a[1]*b[0];
}
/* Moller-Trumbore in long double, with an unnormalized segment direction.
 * The independent literal epsilon is in world units, converted to segment t.
 * Reject test cases near edge/endpoint/parallel boundaries, rather than
 * pretending either implementation is an exact predicate at those boundaries.
 */
static int brute_force(const double *xyz,const double a[3],const double b[3],int *borderline) {
 long double d[3],length,epsilon;unsigned j,k;int blocked=0;
 *borderline=0;
 for(k=0;k<3;k++)d[k]=(long double)b[k]-a[k];
 length=sqrtl(dot(d,d));epsilon=1e-5L/length;
 assert(length>2e-5L);
 for(j=0;j<TRIANGLES;j++) {
  const double *v=xyz+9*j;
  long double e1[3],e2[3],h[3],q[3],r[3],det,u,w,t,scale;
  for(k=0;k<3;k++) {
   e1[k]=(long double)v[3+k]-v[k];e2[k]=(long double)v[6+k]-v[k];
   r[k]=(long double)a[k]-v[k];
  }
  cross(d,e2,h);det=dot(e1,h);
  scale=sqrtl(dot(e1,e1)*dot(e2,e2)*dot(d,d));
  if(fabsl(det)<1e-12L*scale){*borderline=1;continue;}
  u=dot(r,h)/det;cross(r,e1,q);w=dot(d,q)/det;t=dot(e2,q)/det;
  if(fabsl(u)<1e-9L || fabsl(w)<1e-9L || fabsl(1-u-w)<1e-9L ||
     fabsl(t-epsilon)<1e-9L || fabsl(t-(1-epsilon))<1e-9L) *borderline=1;
  if(u>=0 && w>=0 && u+w<=1 && t>epsilon && t<1-epsilon)blocked=1;
 }
 return blocked;
}
int main(void) {
 double xyz[TRIANGLES*9],first_a[3]={0},first_b[3]={0};
 uint32_t ix[TRIANGLES*3];
 vis_scene *scene=NULL;vis_binding binding={0};vis_generation generation={1,1,1};
 vis_limits limits={TRIANGLES,2*TRIANGLES-1,1000000};
 vis_budget budget={{SEGMENTS,1000000,1000000},{0,0,0}};
 unsigned i,j,k,accepted=0,skipped=0,hits=0,misses=0;
 assert(VIS_SEGMENT_EPSILON==1e-5);
 for(i=0;i<TRIANGLES;i++) {
  double center[3];for(k=0;k<3;k++)center[k]=3*random_signed();
  for(j=0;j<3;j++) {
   double p[3];for(k=0;k<3;k++)p[k]=center[k]+random_signed();
   transform(p,xyz+9*i+3*j);ix[3*i+j]=3*i+j;
  }
 }
 assert(vis_scene_build(&scene,xyz,TRIANGLES*3,ix,TRIANGLES,generation,limits)==VIS_OK);
 assert(vis_binding_set(&binding,scene,generation)==VIS_OK);
 assert(vis_scene_triangle_count(scene)==TRIANGLES);
 assert(vis_scene_bytes(scene)==vis_scene_required_bytes_v3(TRIANGLES*3,TRIANGLES));
 while(accepted<SEGMENTS) {
  double a[3],b[3];int borderline,expected,actual=-71;
  assert(skipped<10000);
  if(accepted%2==0) {
   /* Guaranteed interior crossings, with varying oblique triangle normals. */
   const double *v=xyz+9*(accepted/2%TRIANGLES);
   long double e1[3],e2[3],n[3],length;
   for(k=0;k<3;k++){e1[k]=(long double)v[3+k]-v[k];e2[k]=(long double)v[6+k]-v[k];}
   cross(e1,e2,n);length=sqrtl(dot(n,n));
   for(k=0;k<3;k++) {
    double center=(v[k]+v[3+k]+v[6+k])/3;
    a[k]=center-2*(double)(n[k]/length);b[k]=center+2*(double)(n[k]/length);
   }
  } else {
   double p[3],q[3];
   for(k=0;k<3;k++){p[k]=6*random_signed();q[k]=6*random_signed();}
   transform(p,a);transform(q,b);
  }
  expected=brute_force(xyz,a,b,&borderline);
  if(borderline){skipped++;continue;}
  assert(vis_segment_occluded(&binding,generation,a,b,&budget,&actual)==VIS_OK);
  assert(actual==expected);
  if(actual)hits++;else misses++;
  if(accepted==0)for(k=0;k<3;k++){first_a[k]=a[k];first_b[k]=b[k];}
  accepted++;
 }
 assert(hits>=500 && misses>0 && budget.used.rays==SEGMENTS);
 assert(budget.used.node_visits<=budget.limit.node_visits);
 assert(budget.used.triangle_tests<=budget.limit.triangle_tests);
 /* The cumulative ray budget is exhausted, without resetting between rays. */
 {
  int out=71;vis_counters before=budget.used;
  assert(vis_segment_occluded(&binding,generation,first_a,first_b,&budget,&out)==VIS_BUDGET);
  assert(out==71 && budget.used.rays==before.rays && budget.used.node_visits==before.node_visits && budget.used.triangle_tests==before.triangle_tests);
 }
 for(i=0;i<3;i++) {
  vis_budget limited={{1,1000000,1000000},{0,0,0}};int out=71;
  if(i==0)limited.limit.rays=0;
  if(i==1)limited.limit.node_visits=0;
  if(i==2)limited.limit.triangle_tests=0;
  assert(vis_segment_occluded(&binding,generation,first_a,first_b,&limited,&out)==VIS_BUDGET);
  assert(out==71 && limited.used.rays<=limited.limit.rays && limited.used.node_visits<=limited.limit.node_visits && limited.used.triangle_tests<=limited.limit.triangle_tests);
 }
 printf("PASS: independent long-double Moller-Trumbore vs public BVH; triangles=%u segments=%u blocked=%u clear=%u borderline_skipped=%u nodes=%llu triangle_tests=%llu; cumulative and individual budgets atomic\n",TRIANGLES,accepted,hits,misses,skipped,(unsigned long long)budget.used.node_visits,(unsigned long long)budget.used.triangle_tests);
 vis_binding_clear(&binding);vis_scene_release(scene);
 return 0;
}
