#ifndef HONE_NATIVE_DQ_CANDIDATE_H
#define HONE_NATIVE_DQ_CANDIDATE_H
#include <math.h>
#include <float.h>
#include <stddef.h>
#include <stdint.h>
#if defined(__FAST_MATH__) || (defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__ > 0)
#error "hone DQ bridge requires finite checks"
#endif
/* Link the existing tools/hone/source_package/rigid_dq_candidate.c unchanged.
 * Generated geometry header must be included first. All cold spans are trusted,
 * immutable, complete and disjoint from out. Caller supplies SAME-FRAME resolved
 * proper-rigid wxyz+xyz palette, and matching row-major affine 3x4 space maps.
 * No drivers, quaternion conversion, normals or registration are inferred.
 * Position is mesh-local after relative morph evaluation. No allocation or mesh
 * copy. One point commits on success; caller owns unpublished frame and publish.
 */
int rigid_dq_position(const double *,const double *,size_t,const unsigned *,
                      const double *,size_t,double *);
static inline int hone_input_dq_vertex(const hone_input_mesh *mesh,uint32_t vertex,
        const float position[3],const uint32_t *group_to_joint,size_t groups,
        const double *joints,size_t joint_count,const double to_rig[12],
        const double from_rig[12],float out[3]) {
    if(!mesh||!position||!out||!mesh->weight_ranges||!mesh->weights||
       vertex>=mesh->vertices||groups!=mesh->groups||!group_to_joint||
       !joint_count||joint_count>65536||!joints||!to_rig||!from_rig)return 0;
    hone_input_range range=mesh->weight_ranges[vertex];
    if(range.count>4||range.first>UINT32_MAX-range.count)return 0;
    unsigned indices[4];double weights[4];
    for(size_t k=0;k<range.count;k++) {
        hone_input_weight w=mesh->weights[range.first+k];
        if(w.group>=groups||group_to_joint[w.group]>=joint_count||
           !isfinite(w.weight)||w.weight<0)return 0;
        indices[k]=group_to_joint[w.group];weights[k]=w.weight;
    }
    for(unsigned k=0;k<3;k++)if(!isfinite(position[k]))return 0;
    for(unsigned k=0;k<12;k++)
        if(!isfinite(to_rig[k])||!isfinite(from_rig[k]))return 0;
    double input[3],deformed[3];float result[3];
    for(unsigned r=0;r<3;r++) {
        input[r]=to_rig[4*r]*position[0]+to_rig[4*r+1]*position[1]+
                 to_rig[4*r+2]*position[2]+to_rig[4*r+3];
        if(!isfinite(input[r]))return 0;
    }
    if(!rigid_dq_position(input,joints,joint_count,indices,weights,range.count,deformed))return 0;
    for(unsigned r=0;r<3;r++) {
        double v=from_rig[4*r]*deformed[0]+from_rig[4*r+1]*deformed[1]+
                 from_rig[4*r+2]*deformed[2]+from_rig[4*r+3];
        if(!isfinite(v)||fabs(v)>FLT_MAX)return 0;
        result[r]=(float)v;
    }
    for(unsigned k=0;k<3;k++)out[k]=result[k];
    return 1;
}
#endif
