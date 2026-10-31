#ifndef AYATSURI_VISIBILITY_H
#define AYATSURI_VISIBILITY_H
#include <stddef.h>
#include <stdint.h>

#define AYATSURI_VISIBILITY_VERTICES 32u
#define AYATSURI_VISIBILITY_TRIANGLES 64u

typedef enum ayatsuri_visibility_order {
    AYATSURI_VISIBILITY_AUTHORED=0,
    AYATSURI_VISIBILITY_CENTROID_DEPTH=1
} ayatsuri_visibility_order;
typedef enum ayatsuri_visibility_status {
    AYATSURI_VISIBILITY_OK,
    AYATSURI_VISIBILITY_INVALID,
    AYATSURI_VISIBILITY_RANGE,
    AYATSURI_VISIBILITY_OVERLAP
} ayatsuri_visibility_status;

/* Caller-owned, naturally aligned arrays, borrowed only during this call.
 * vertices: 1..32; triangles: 1..64; capacity counts uint16_t elements and
 * must cover 3*triangles. Every declared output byte must be disjoint from
 * both outputs and all inputs. No allocation, retained pointer or globals.
 * XY is finite F32 in [-4096,4096]; depth is finite F32, larger is nearer.
 * Binary64 oriented area >1e-5 is front; <-1e-5 is back; otherwise degenerate.
 * Front triangles retain their source winding. CENTROID_DEPTH sorts by the
 * ascending binary64 sum of three depths, then ascending source triangle ID.
 * This is a painter heuristic, not a Z-buffer or general visibility proof.
 * Success writes only out[0..3*visible), then counts[visible,back,degenerate].
 * Any failure leaves all output/count bytes unchanged; unused tail is never
 * written. Compile with IEEE arithmetic and -ffp-contract=off, no fast-math. */
#ifdef __cplusplus
extern "C" {
#endif
ayatsuri_visibility_status ayatsuri_visibility_select(
    const float *xy,const float *depth,size_t vertices,
    const uint16_t *indices,size_t triangles,ayatsuri_visibility_order order,
    uint16_t *out,size_t capacity,uint32_t counts[3]);
#ifdef __cplusplus
}
#endif
#endif
