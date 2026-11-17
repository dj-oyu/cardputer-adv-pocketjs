#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "nearest_batch_offset_candidate.c"

int main(void){
 double vertices[]={-2,-2,0,2,-2,0,2,2,0,-2,2,0};
 uint32_t triangles[]={0,1,2,0,2,3};
 double points[]={.2,.3,1,-.4,.1,-1,0,0,0,2,2,.2};
 double result[12],reference[3],before[12];uint32_t faces[4];nearest_box boxes[1];size_t tests;
 assert(nearest_batch_offset(points,4,vertices,4,triangles,2,.125,boxes,1,result,faces,&tests));
 for(unsigned i=0;i<4;i++){
  assert(nearest_surface_offset(points+3*i,vertices,4,triangles,2,.125,reference));
  for(unsigned k=0;k<3;k++)assert(fabs(result[3*i+k]-reference[k])<1e-14);
 }
 memcpy(before,result,sizeof result);
 assert(!nearest_batch_offset(points,4,vertices,4,triangles,2,NAN,boxes,1,result,faces,&tests));
 assert(!memcmp(before,result,sizeof result));
 assert(!nearest_batch_offset(points,SIZE_MAX/(3*sizeof(double))+1,vertices,4,triangles,2,.125,boxes,1,result,faces,&tests));
 assert(!nearest_batch_offset(points,4,vertices,SIZE_MAX/(3*sizeof(double))+1,triangles,2,.125,boxes,1,result,faces,&tests));
 assert(!nearest_batch_offset(points,4,vertices,4,triangles,SIZE_MAX/(3*sizeof(uint32_t))+1,.125,boxes,1,result,faces,&tests));
 assert(!memcmp(before,result,sizeof result));
 points[11]=NAN;
 assert(!nearest_batch_offset(points,4,vertices,4,triangles,2,.125,boxes,1,result,faces,&tests));
 points[11]=.2;
 assert(nearest_batch_offset(points,4,vertices,4,triangles,2,.125,boxes,1,result,faces,&tests));
 for(unsigned i=0;i<12;i++)assert(result[i]==before[i]);
 puts("PASS batched ON_SURFACE offset, both sides, coincidence and retry");
 return 0;
}
