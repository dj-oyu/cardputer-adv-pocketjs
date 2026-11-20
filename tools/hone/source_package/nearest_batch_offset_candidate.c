/* Host-only ON_SURFACE candidate; never a runtime admission on its own.
 * Include the existing batch implementation once in this translation unit. */
#if defined(__FAST_MATH__) || (defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__ > 0)
#error "nearest batch offset requires finite checks"
#endif
#include "nearest_batch_candidate.c"

/* points and vertices use the same already-evaluated target coordinate space.
 * Offset is in that space. Caller must establish compatible transforms and the
 * order of morph/rig/wrap, and provide exclusive, complete, disjoint arrays.
 * output is unpublished candidate scratch; failure may leave partial output.
 * No cache survives the call; nearest_batch validates/refits once per batch. */
int nearest_batch_offset(const double *points,size_t count,
    const double *vertices,size_t vertex_count,const uint32_t *triangles,
    size_t triangle_count,double offset,nearest_box *boxes,size_t box_capacity,
    double *output,uint32_t *faces,size_t *tests) {
 if(!isfinite(offset)||count>SIZE_MAX/(3*sizeof(double))||
    vertex_count>SIZE_MAX/(3*sizeof(double))||
    triangle_count>SIZE_MAX/(3*sizeof(uint32_t))||triangle_count>SIZE_MAX-7)return 0;
 if(!nearest_batch(points,count,vertices,vertex_count,triangles,triangle_count,
                   boxes,box_capacity,output,faces,tests))return 0;
 for(size_t i=0;i<count;i++){
  double d[3],len2=0;
  for(unsigned k=0;k<3;k++){d[k]=points[3*i+k]-output[3*i+k];len2+=d[k]*d[k];}
  double length=sqrt(len2);
  if(!isfinite(length))return 0;
  if(length>0)for(unsigned k=0;k<3;k++){
   double candidate=output[3*i+k]+offset*d[k]/length;
   if(!isfinite(candidate))return 0;
   output[3*i+k]=candidate;
  }
 }
 return 1;
}
