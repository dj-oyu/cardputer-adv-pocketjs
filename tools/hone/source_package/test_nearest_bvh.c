#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nearest_surface_candidate.c"
#include "nearest_bvh_candidate.h"
static double kernel(const double*p,const double*a,const double*b,const double*c,double*out){
 double w[3],d=nearest_triangle(p,a,b,c,w);for(unsigned k=0;k<3;k++)out[k]=w[0]*a[k]+w[1]*b[k]+w[2]*c[k];return d;
}
static uint32_t rng=17;
static double random_coord(void){rng=rng*1664525+1013904223;return ((int)(rng>>8)%2049-1024)/32.;}
int main(void){
 enum{N=257};double vertices[N*9];uint32_t triangles[N*3];
 for(unsigned i=0;i<N*9;i++)vertices[i]=random_coord();for(unsigned i=0;i<N*3;i++)triangles[i]=i;
 /* Duplicate triangles force equal-distance original-order decisions. */
 memcpy(vertices+9,vertices,9*sizeof(double));
 size_t bytes=hone_nearest_bvh_bytes(N);void*work=malloc(bytes);assert(work);hone_nearest_bvh b;
 assert(!hone_nearest_bvh_build(&b,work,bytes-1,vertices,N*3,triangles,N));
 size_t tested=0,visited=0;
 for(unsigned frame=0;frame<3;frame++){
  if(frame)for(unsigned j=0;j<N*9;j++)vertices[j]+=.03125;
  assert(hone_nearest_bvh_build(&b,work,bytes,vertices,N*3,triangles,N));
  for(unsigned sample=0;sample<1000;sample++){
   double p[3]={random_coord(),random_coord(),random_coord()},hit[3],reference[3]={0},best=INFINITY;uint32_t face=UINT32_MAX,winner=0;
   if(sample==0)memcpy(p,vertices,3*sizeof(double));
   if(sample==1)p[0]+=0x1p-40; /* non-binary32 query fallback */
   for(unsigned f=0;f<N;f++){double q[3],d=kernel(p,vertices+9*f,vertices+9*f+3,vertices+9*f+6,q);if(d<best){best=d;winner=f;memcpy(reference,q,sizeof q);}}
   assert(hone_nearest_bvh_query(&b,p,kernel,hit,&face,&tested,&visited));assert(face==winner&&!memcmp(hit,reference,sizeof hit));
  }
 }
 double bad[3]={NAN,0,0},out[3]={1,2,3};uint32_t face=99;assert(!hone_nearest_bvh_query(&b,bad,kernel,out,&face,&tested,&visited));assert(out[0]==1&&face==99);
 vertices[0]=INFINITY;assert(!hone_nearest_bvh_build(&b,work,bytes,vertices,N*3,triangles,N));vertices[0]=0;triangles[0]=N*3;assert(!hone_nearest_bvh_build(&b,work,bytes,vertices,N*3,triangles,N));
 assert(!hone_nearest_bvh_bytes(65536));free(work);
 printf("BVH PASS 3000 brute comparisons ties/refit/nonfloat/invalid capacity; tests=%zu visits=%zu\n",tested,visited);return 0;
}
