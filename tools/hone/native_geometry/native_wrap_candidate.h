#ifndef HONE_NATIVE_WRAP_CANDIDATE_H
#define HONE_NATIVE_WRAP_CANDIDATE_H
#include <stddef.h>
#include <stdint.h>
/* Host candidate only; all input and output spans must be complete, immutable
 * or exclusively owned, and pairwise disjoint as documented by the C file. */
size_t hone_native_wrap_box_bytes(size_t triangle_count);
int hone_native_relative_transform(const float target[12],const float source[12],double output[12]);
int hone_native_wrap(const float *source,size_t source_count,const float *target,size_t target_count,
 const uint32_t *triangles,size_t triangle_count,const double mat[12],double offset,
 double *points,size_t point_capacity,double *vertices,size_t vertex_capacity,
 double *hits,size_t hit_capacity,uint32_t *faces,size_t face_capacity,
 void *boxes,size_t box_capacity,float *output,size_t output_capacity,size_t *tests);
#endif
