#ifndef HONE_NATIVE_NORMALS_BATCH_CANDIDATE_H
#define HONE_NATIVE_NORMALS_BATCH_CANDIDATE_H
#include <stddef.h>
#include <stdint.h>
/* Include generated geometry first. Trusted complete immutable descriptors,
 * positions and topology. All output spans and workspace must be disjoint from
 * each other and from every input/descriptor for the full synchronous call.
 * Job descriptors and the shared work arena are caller-owned storage, not
 * variable-sized arrays on the render task stack. Scratch output is unpublished: kernel failure can leave a partial batch.
 * No callbacks to presentation, no publication and no allocation. Kernel is a
 * trusted normal implementation, not a user callback. Whole meshes are retained;
 * never compute smoothing separately for provider partitions. */
typedef int (*hone_normal_kernel)(const float*,size_t,const uint32_t*,const uint32_t*,const uint8_t*,size_t,size_t,double*,float*);
typedef struct {const hone_input_mesh *mesh;const float *positions;size_t position_values;float *normals;size_t normal_values;} hone_normal_job;
static inline int hone_normals_batch(const hone_normal_job *jobs,size_t count,
    double *work,size_t work_values,hone_normal_kernel kernel){
 if(!jobs||!work||!kernel||count>1024)return 0;
 /* Validate every declared span before any workspace or output write. */
 for(size_t i=0;i<count;i++){
  const hone_normal_job *j=jobs+i;const hone_input_mesh *m=j->mesh;
  if(!m||!j->positions||!j->normals||!m->polygon_offsets||!m->polygon_vertices||!m->polygon_smooth||m->source_vertex_count>1000000||m->polygon_corner_count>1000000)return 0;
  if(m->source_vertex_count>j->position_values/3||m->source_vertex_count>work_values/3||m->polygon_corner_count>j->normal_values/3)return 0;
 }
 for(size_t i=0;i<count;i++){
  const hone_normal_job *j=jobs+i;const hone_input_mesh *m=j->mesh;
  if(!kernel(j->positions,m->source_vertex_count,m->polygon_offsets,m->polygon_vertices,m->polygon_smooth,m->polygon_count,m->polygon_corner_count,work,j->normals))return 0;
 }
 return 1;
}
#endif
