#ifndef HONE_DRIVER_NODE_TARGETS_CANDIDATE_H
#define HONE_DRIVER_NODE_TARGETS_CANDIDATE_H
#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "ayatsuri/ayatsuri_model.h"
#if defined(__FAST_MATH__) || (defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__ > 0)
#error "driver node targets require IEEE finite checks"
#endif
/* This candidate stages only admitted object-pose and hide_render outputs.
 * All remaining shape/rig/shader routes must succeed before frame publication.
 * Routes, values and base_pose are complete, immutable and pairwise disjoint
 * from output spans. Caller owns and synchronizes all arrays; no aliases.
 * Pose routes require explicit source-specific direct-XYZ admission. All
 * validation precedes output writes; failure leaves both output spans intact. */
enum { HONE_DRIVER_HIDE_RENDER = 9 };
typedef struct { uint16_t program, node; uint8_t channel; } hone_driver_node_route;
static inline int hone_driver_node_targets(const hone_driver_node_route *routes,size_t route_count,
    const double *values,size_t value_count,const float *base_pose,size_t node_count,
    uint8_t *dirty,float *pose) {
    if(node_count>256||route_count>4096||!base_pose||!dirty||!pose||
       (route_count&&(!routes||!values)))return 0;
    for(size_t i=0;i<node_count;i++)for(unsigned k=0;k<9;k++){
        float v=base_pose[i*9+k];if(!isfinite(v)||(k>=6&&v==0))return 0;
    }
    for(size_t i=0;i<route_count;i++){
        hone_driver_node_route r=routes[i];
        if(r.node>=node_count||r.program>=value_count||r.channel>HONE_DRIVER_HIDE_RENDER)return 0;
        double value=values[r.program];
        if(!isfinite(value)||fabs(value)>FLT_MAX)return 0;
        if(r.channel==HONE_DRIVER_HIDE_RENDER){if(value!=0&&value!=1)return 0;}
        else if(r.channel>=6&&(float)value==0)return 0;
        for(size_t j=0;j<i;j++)if(routes[j].node==r.node&&routes[j].channel==r.channel)return 0;
    }
    memcpy(pose,base_pose,node_count*9*sizeof(*pose));memset(dirty,0,node_count);
    for(size_t i=0;i<route_count;i++){
        hone_driver_node_route r=routes[i];double value=values[r.program];
        if(r.channel==HONE_DRIVER_HIDE_RENDER)
            dirty[r.node]|=value==0?AYATSURI_TARGET_VISIBLE:AYATSURI_TARGET_HIDDEN;
        else {
            pose[(size_t)r.node*9+r.channel]=(float)value;
            dirty[r.node]|=(uint8_t)(1u<<(r.channel/3));
        }
    }
    return 1;
}
#endif
