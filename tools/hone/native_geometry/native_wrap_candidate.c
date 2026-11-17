/* Host candidate for an admitted nearest-surface ON_SURFACE wrap. This does
 * not establish modifier order, rig state, frame publication, or target speed. */
#include "../source_package/nearest_batch_offset_candidate.c"
#include <float.h>
#include <stdint.h>

/* All spans are trusted complete, pairwise disjoint and exclusively owned.
 * Mat is source-to-target affine, row-major 3x4. Rigid transforms only:
 * nonuniform scaling would change nearest distances and offset semantics.
 * Workspace and output are unpublished and may be partially written on error.
 * Boxes must be suitably aligned for float. Capacities are in elements. */
int hone_native_wrap(const float *source,size_t source_count,const float *target,size_t target_count,
 const uint32_t *triangles,size_t triangle_count,const double mat[12],double offset,
 double *points,size_t point_capacity,double *vertices,size_t vertex_capacity,
 double *hits,size_t hit_capacity,uint32_t *faces,size_t face_capacity,
 void *boxes,size_t box_capacity,float *output,size_t output_capacity,size_t *tests){
 if(!source||!target||!triangles||!mat||!points||!vertices||!hits||!faces||!boxes||!output||!tests||
    !source_count||!target_count||!triangle_count||source_count>SIZE_MAX/(3*sizeof(double))||
    target_count>SIZE_MAX/(3*sizeof(double))||triangle_count>SIZE_MAX/(3*sizeof(uint32_t))||
    triangle_count>SIZE_MAX-7||point_capacity<source_count*3||vertex_capacity<target_count*3||
    hit_capacity<source_count*3||face_capacity<source_count||output_capacity<source_count*3||
    box_capacity<(triangle_count+7)/8||(uintptr_t)boxes%_Alignof(nearest_box)||!isfinite(offset))return 0;
 for(unsigned i=0;i<12;i++)if(!isfinite(mat[i]))return 0;
 for(unsigned a=0;a<3;a++)for(unsigned b=0;b<3;b++){
  double dot=0;for(unsigned k=0;k<3;k++)dot+=mat[k*4+a]*mat[k*4+b];
  if(!isfinite(dot)||fabs(dot-(a==b?1.:0.))>1e-5)return 0;
 }
 const double a=mat[0],b=mat[1],c=mat[2],d=mat[4],e=mat[5],f=mat[6],g=mat[8],h=mat[9],i=mat[10];
 const double det=a*(e*i-f*h)-b*(d*i-f*g)+c*(d*h-e*g);
 if(!isfinite(det)||fabs(det)<1e-12)return 0;
 const double inverse[9]={(e*i-f*h)/det,(c*h-b*i)/det,(b*f-c*e)/det,
                          (f*g-d*i)/det,(a*i-c*g)/det,(c*d-a*f)/det,
                          (d*h-e*g)/det,(b*g-a*h)/det,(a*e-b*d)/det};
 for(size_t n=0;n<source_count;n++)for(unsigned r=0;r<3;r++){
  double v=mat[r*4+3];for(unsigned k=0;k<3;k++){if(!isfinite(source[n*3+k]))return 0;v+=mat[r*4+k]*(double)source[n*3+k];}
  if(!isfinite(v))return 0;points[n*3+r]=v;
 }
 for(size_t n=0;n<target_count*3;n++){if(!isfinite(target[n]))return 0;vertices[n]=target[n];}
 if(!nearest_batch_offset(points,source_count,vertices,target_count,triangles,triangle_count,offset,
                          (nearest_box*)boxes,box_capacity,hits,faces,tests))return 0;
 for(size_t n=0;n<source_count;n++)for(unsigned r=0;r<3;r++){
  double v=0;for(unsigned k=0;k<3;k++)v+=inverse[r*3+k]*(hits[n*3+k]-mat[k*4+3]);
  if(!isfinite(v)||fabs(v)>FLT_MAX)return 0;output[n*3+r]=(float)v;
 }
 return 1;
}
