#ifndef KSN_MESH_PREPARED_H
#define KSN_MESH_PREPARED_H
#include "ksn_mesh_depth.h"
#ifdef KSN_MESH_PREPARED_EDGE4
#include "pie_edge4.h"
_Static_assert(KSN_PROC_W==240 && KSN_PROC_H==135,"edge4 range proof requires 240x135");
#ifdef KSN_MESH_EDGE4_HOST_STATS
extern uint64_t ksn_mesh_prepared_edge4_stats[4]; /* blocks, empty, pixels, rejects */
#endif
#endif
/* Private opt-in prototype. No source pointers, no PSRAM, no numeric changes.
 * Caller owns allocation, keeps committed separate, and swaps only on ACK.
 * A failed build invalidates only the supplied candidate, never committed.
 * Source/output/scratch/pixels must be disjoint as in the original renderer. */
enum { KSN_PREPARED_MAX_TRIANGLES=64, KSN_PREPARED_BANDS=17 };
typedef struct {double a,b,c;int sign;bool closed;} ksn_prepared_edge;
typedef struct {
    co_vertex v[3]; ksn_prepared_edge edge[3]; double inverse;
    int left,right,top,bottom;
#ifdef KSN_MESH_PREPARED_EDGE4
    pie_edge4_coeff classify;bool classify_valid;
#endif
} ksn_prepared_triangle;
typedef struct {
    uint64_t bands[KSN_PREPARED_BANDS];
    unsigned capacity,count; bool valid;
    ksn_prepared_triangle triangles[];
} ksn_mesh_prepared;
/* Sizing validates packets but does not decode/setup triangles. Zero on error
 * or >64 triangles: integration falls back to original raster, never drops. */
size_t ksn_mesh_prepared_bytes(const ksn_mesh_snapshot *source);
bool ksn_mesh_prepare(const ksn_mesh_snapshot *source,ksn_mesh_prepared *out,size_t bytes);
bool ksn_mesh_prepared_render(const ksn_mesh_prepared *,uint16_t *,unsigned,unsigned,float *,unsigned);
#endif
