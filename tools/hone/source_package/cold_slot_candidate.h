#ifndef HONE_COLD_SLOT_CANDIDATE_H
#define HONE_COLD_SLOT_CANDIDATE_H
/* Host integration candidate. Cold maps retain original vertex AND corner
 * identity; never weld corners by equal rest normal. No allocation/cache. */
#include "ayatsuri/ayatsuri_model_provider.h"
typedef struct { uint32_t vertex,corner; } hone_cold_corner;
/* Source/mapping spans must be live, disjoint from slot and immutable under
 * the caller's exclusive candidate-frame ownership. Generations identify the
 * same evaluated pose, not pointer reuse. Cold model identity is bound by the
 * scene adapter before invoking this function, not inferred from these arrays.
 * Failure may modify candidate-local slot only; never publish a failed frame. */
static inline bool hone_cold_slot(const float *positions,size_t nv,
 const float *corner_normals,size_t nc,const hone_cold_corner *map,size_t count,
 uint32_t position_generation,uint32_t normal_generation,uint32_t frame_generation,
 ayatsuri_skin_result *slot){
 if(!positions||!corner_normals||!map||!slot||!nv||!nc||!count||count>32||
    nv>SIZE_MAX/3||nc>SIZE_MAX/3||!frame_generation||position_generation!=frame_generation||normal_generation!=frame_generation)return false;
 for(size_t i=0;i<count;i++){
  if(map[i].vertex>=nv||map[i].corner>=nc)return false;
  float squared=0;
  for(unsigned k=0;k<3;k++){
   float p=positions[3*(size_t)map[i].vertex+k],n=corner_normals[3*(size_t)map[i].corner+k];
   if(!isfinite(p)||p < -4096||p >4096||!isfinite(n))return false;
   slot[i].position[k]=p;slot[i].normal[k]=n;squared+=n*n;
  }
  if(!isfinite(squared)||fabsf(squared-1)>1e-4f)return false;
 }
 return true;
}
#endif
