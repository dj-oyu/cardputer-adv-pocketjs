#ifndef HONE_NATIVE_MORPH_CANDIDATE_H
#define HONE_NATIVE_MORPH_CANDIDATE_H
#include <math.h>
#include <stddef.h>
/* Include the generated geometry header first. This candidate reads immutable
 * generated arrays only. The caller resolves Blender drivers and per-vertex
 * masks in source key order; no defaults silently drop a masked shape.
 * No allocation, no mesh copy, and a single point commits only on success.
 * Output must be writable scratch disjoint from cold arrays and controls.
 * Not skinning, normal recomputation or runtime registration. */
static inline int hone_input_morph_position(const hone_input_mesh *m,
    uint32_t vertex, const float *values, const float *masks, size_t controls,
    float out[3]) {
    if (!m || !out || !m->position || vertex >= m->vertices ||
        controls != m->morphs || (controls && !m->relative_morph) || (controls && (!values || !masks ||
        !m->morph_positions || !m->morph_relative))) return 0;
    /* Generated arrays are trusted cold data, not arbitrary pointer inputs.
     * Bound arithmetic before accessing key-major xyz arrays. */
    if (m->vertices > 1000000 || m->morphs > 4096 ||
        (m->morphs && m->vertices > SIZE_MAX / (3 * sizeof(float)) / m->morphs)) return 0;
    float result[3];
    for (unsigned j=0;j<3;j++) {
        result[j]=m->position[(size_t)vertex*3+j];
        if (!isfinite(result[j])) return 0;
    }
    for (size_t key=0;key<controls;key++) {
        uint32_t relative=m->morph_relative[key];
        if (relative >= m->morphs || !isfinite(values[key]) ||
            !isfinite(masks[key]) || masks[key]<0 || masks[key]>1) return 0;
        float amount=values[key]*masks[key];
        if (!isfinite(amount)) return 0;
        for (unsigned j=0;j<3;j++) {
            size_t index=(key*m->vertices+vertex)*3+j;
            size_t base=((size_t)relative*m->vertices+vertex)*3+j;
            float target=m->morph_positions[index];
            float origin=m->morph_positions[base];
            if (!isfinite(target) || !isfinite(origin)) return 0;
            result[j]+=amount*(target-origin);
            if (!isfinite(result[j])) return 0;
        }
    }
    for (unsigned j=0;j<3;j++) out[j]=result[j];
    return 1;
}
#endif
