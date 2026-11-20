#ifndef HONE_NATIVE_DEFORM_CANDIDATE_H
#define HONE_NATIVE_DEFORM_CANDIDATE_H
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#if defined(__FAST_MATH__) || (defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__ > 0)
#error "hone deform requires finite checks"
#endif
/* Include generated geometry, native_morph.h and native_dq.h first.
 * Controls and masks are already resolved in the same frame, in source key
 * order. masks is source_vertex_count by controls, vertex-major. skin is
 * explicitly selected by the caller after modifier admission. Output is
 * unpublished, disjoint caller scratch; failure may leave a partial result.
 * Isolated vertices have no representative and are copied unchanged. They
 * must not participate in polygons or modifier dependencies. This candidate
 * does not evaluate drivers, shrinkwrap, normals or modifier ordering. */
static inline int hone_deform_source(const hone_input_mesh *m,
    const float *values,const float *masks,size_t controls,int skin,
    const uint32_t *group_to_joint,size_t groups,const double *joints,
    size_t joint_count,const double to_rig[12],const double from_rig[12],
    float *output) {
    if(!m||!output||!m->source_position||!m->source_representative||
       !m->source_vertex||m->source_vertex_count>1000000||
       controls!=m->morphs||controls>4096||(controls&&(!values||!masks))||
       (controls && m->source_vertex_count>SIZE_MAX/sizeof(*masks)/controls) ||
       (skin!=0 && skin!=1))return 0;
    for(uint32_t s=0;s<m->source_vertex_count;s++){
        uint32_t v=m->source_representative[s];float point[3];
        if(v==UINT32_MAX){
            for(unsigned k=0;k<3;k++){
                point[k]=m->source_position[(size_t)s*3+k];
                if(!isfinite(point[k]))return 0;
            }
        }else{
            if(v>=m->vertices||m->source_vertex[v]!=s||
               !hone_input_morph_position(m,v,values,controls?masks+(size_t)s*controls:NULL,controls,point))return 0;
            if(skin){
                float skinned[3];
                if(!hone_input_dq_vertex(m,v,point,group_to_joint,groups,joints,joint_count,to_rig,from_rig,skinned))return 0;
                for(unsigned k=0;k<3;k++)point[k]=skinned[k];
            }
        }
        for(unsigned k=0;k<3;k++)output[(size_t)s*3+k]=point[k];
    }
    return 1;
}
#endif
