#ifndef HONE_NATIVE_GATHER_CANDIDATE_H
#define HONE_NATIVE_GATHER_CANDIDATE_H
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#if defined(__FAST_MATH__) || (defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__ > 0)
#error "hone gather requires finite checks"
#endif

/* Include the generated geometry descriptor first. Full-mesh positions and
 * packed polygon-corner normals belong to one unpublished frame. A part maps
 * into prepared corners, whose source loop IDs need not be contiguous. All
 * inputs and output are disjoint; failure leaves output untouched. The
 * descriptor and referenced spans must be complete, valid and immutable. This
 * is not an untrusted-data parser or admission of the normal generator. */
typedef struct { float position[3], normal[3]; } hone_gather_vertex;
typedef struct { uint32_t source, normal; } hone_gather_index;
/* Internal validation pass. The index plan is private scratch and may be
 * partially written on failure. Source values must stay immutable through the
 * subsequent copy; a plan is never retained or reused across calls/frames. */
static inline int hone_gather_plan(const hone_input_mesh *m,
    const uint32_t *prepared, size_t count, const float *source_positions,
    const float *packed_normals, hone_gather_index *plan) {
    if (!m || !prepared || !source_positions || !packed_normals || !plan ||
        !m->source_vertex || !m->source_corner || !m->corner_polygon ||
        !m->polygon_offsets || !m->polygon_corners || count > 32 ||
        m->vertices > 1000000 || m->source_vertex_count > 1000000 ||
        m->polygon_corner_count > 3000000 || m->source_corner_count > 3000000 ||
        m->polygon_count > 1000000) return 0;
    for (size_t i=0; i<count; ++i) {
        uint32_t v=prepared[i];
        if (v>=m->vertices) return 0;
        uint32_t source=m->source_vertex[v], loop=m->source_corner[v], face=m->corner_polygon[v];
        if (source>=m->source_vertex_count || loop>=m->source_corner_count || face>=m->polygon_count) return 0;
        uint32_t start=m->polygon_offsets[face], end=m->polygon_offsets[face+1];
        if (start>end || end>m->polygon_corner_count || end-start<3 || end-start>4) return 0;
        size_t found=end;
        for (size_t j=start; j<end; ++j) if (m->polygon_corners[j]==loop) {
            if (found!=end) return 0;
            found=j;
        }
        if (found==end) return 0;
        double norm=0;
        for (size_t j=0; j<3; ++j) {
            float p=source_positions[(size_t)source*3+j], n=packed_normals[found*3+j];
            if (!isfinite(p) || fabsf(p)>4096 || !isfinite(n)) return 0;
            norm+=(double)n*n;
        }
        if (fabs(norm-1.0)>1e-4) return 0;
        plan[i].source=source; plan[i].normal=(uint32_t)found;
    }
    return 1;
}
static inline int hone_gather_part(const hone_input_mesh *m,
    const uint32_t *prepared, size_t count, const float *source_positions,
    const float *packed_normals, hone_gather_vertex *out) {
    hone_gather_index plan[32];
    if (!out || !hone_gather_plan(m,prepared,count,source_positions,packed_normals,plan)) return 0;
    for (size_t i=0; i<count; ++i) for (size_t k=0;k<3;++k) {
        out[i].position[k]=source_positions[(size_t)plan[i].source*3+k];
        out[i].normal[k]=packed_normals[(size_t)plan[i].normal*3+k];
    }
    return 1;
}
#endif
