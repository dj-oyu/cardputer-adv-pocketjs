/* Independent host-only accelerator. No retained geometry or cross-call cache. */
#include "nearest_surface_candidate.c"
#include <float.h>
typedef struct { float lo[3],hi[3]; } nearest_box;
/* Inputs/work/output are disjoint and exclusively owned for this call. Output
 * is a candidate array: failure may leave earlier points written, never publish
 * it on failure. Refit happens on EVERY call, including same pointer geometry. */
int nearest_batch(const double *points,size_t np,const double *vertices,size_t nv,
 const uint32_t *triangles,size_t nt,nearest_box *boxes,size_t capacity,
 double *output,uint32_t *faces,size_t *tests){
 if(!points||!vertices||!triangles||!output||!faces||!tests||!np||!nv||!nt||nt>UINT32_MAX||nt>SIZE_MAX/3||nv>SIZE_MAX/3||np>SIZE_MAX/3)return 0;
 *tests=0;size_t nb=(nt+7)/8;int accelerate=boxes&&capacity>=nb;double scale=1;
 for(size_t f=0;f<nt;f++)for(unsigned j=0;j<3;j++){
  uint32_t i=triangles[3*f+j];if(i>=nv)return 0;
  for(unsigned k=0;k<3;k++){double x=vertices[3*i+k];if(!isfinite(x))return 0;if(fabs(x)>scale)scale=fabs(x);}
 }
 /* Extreme magnitudes use original brute-force semantics, never unsafe boxes. */
 if(scale>1e10)accelerate=0;
 if(accelerate)for(size_t b=0;b<nb;b++){
  double lo[3]={INFINITY,INFINITY,INFINITY},hi[3]={-INFINITY,-INFINITY,-INFINITY};
  for(size_t f=b*8;f<nt&&f<b*8+8;f++)for(unsigned j=0;j<3;j++)for(unsigned k=0;k<3;k++){
   double x=vertices[3*triangles[3*f+j]+k];if(x<lo[k])lo[k]=x;if(x>hi[k])hi[k]=x;
  }
  for(unsigned k=0;k<3;k++){boxes[b].lo[k]=nextafterf((float)lo[k],-INFINITY);boxes[b].hi[k]=nextafterf((float)hi[k],INFINITY);}
 }
 for(size_t n=0;n<np;n++){
  const double *p=points+3*n;double s=scale;for(unsigned k=0;k<3;k++){if(!isfinite(p[k]))return 0;if(fabs(p[k])>s)s=fabs(p[k]);}
  double best=INFINITY,winner[3]={0};uint32_t which=0;
  for(size_t b=0;b<nb;b++){
   if(accelerate&&s<=1e10){double bound=0;for(unsigned k=0;k<3;k++){double d=fmax(fmax((double)boxes[b].lo[k]-p[k],p[k]-(double)boxes[b].hi[k]),0);bound+=d*d;}
    /* Deliberately loose rounding allowance; equality is never skipped. */
    if(bound>best+128*DBL_EPSILON*s*s)continue;
   }
   for(size_t f=b*8;f<nt&&f<b*8+8;f++){
    const uint32_t*t=triangles+3*f;double w[3];double d=nearest_triangle(p,vertices+3*t[0],vertices+3*t[1],vertices+3*t[2],w);(*tests)++;
    if(d<best){best=d;which=(uint32_t)f;for(unsigned k=0;k<3;k++)winner[k]=w[k];}
   }
  }
  if(!isfinite(best))return 0;double q[3]={0};for(unsigned k=0;k<3;k++)for(unsigned j=0;j<3;j++)q[k]+=winner[j]*vertices[3*triangles[3*which+j]+k];
  for(unsigned k=0;k<3;k++)if(!isfinite(q[k]))return 0;
  for(unsigned k=0;k<3;k++)output[3*n+k]=q[k];faces[n]=which;
 }
 return 1;
}
