#ifndef AYATSURI_MODEL_STREAM_CANDIDATE_H
#define AYATSURI_MODEL_STREAM_CANDIDATE_H
#include "ayatsuri_model_provider.h"
#include "ayatsuri_visibility.h"
#include "../ui/kasane/ksn_mesh_depth.h"
/* Explicit native-only prototype. Compile ayatsuri_model_stream_candidate.c
 * separately; not a default model API, wire flag, JS entry or runtime admission.
 * Model/blob are borrowed immutable until dispose. Instance must start zeroed.
 * Do not access core or pass it to ordinary APIs. No retained mesh view exists. */
typedef struct {ayatsuri_model_instance core;uint32_t tag;} ayatsuri_stream_candidate;
enum { AYATSURI_STREAM_OPAQUE=0, AYATSURI_STREAM_MATERIAL=1 };
typedef struct {uint16_t node_id,mesh_id,material_id;uint8_t lane;} ayatsuri_stream_route;
typedef struct {
 const ayatsuri_model *model;
 const ayatsuri_stream_route *routes;
 size_t count;
} ayatsuri_stream_routes;
/* Synchronous borrows valid ONLY during this callback. Copy all needed values;
 * never retain pointers, mutate inputs, reenter or invoke script. local is the
 * current provider's deformed object-local XYZ/normal, not reconstructed from
 * screen coordinates. projected and depth use the evaluator's exact arithmetic.
 * source_triangle[t] names the authored cold-mesh triangle BEFORE visibility
 * selection/depth ordering. Hidden meshes have triangle_count=0 and NULL indices.
 * Material lane copies into caller-owned pending payload, never visible READY. */
typedef struct {
 uint16_t node_id,mesh_id,material_id;
 uint8_t lane;bool enabled;
 unsigned vertex_count,triangle_count,authored_triangle_count;
 const ayatsuri_skin_result *local;
 const ksn_proc_gouraud_vertex *projected;
 const float *depth;
 const uint16_t *indices,*source_triangle;
} ayatsuri_stream_attributed_mesh;
typedef bool (*ayatsuri_stream_attributed_sink)(void*,const ayatsuri_stream_attributed_mesh*);
/* Open immutable group/mesh provider-only capability; out unchanged on failure. */
ayatsuri_model_status ayatsuri_stream_candidate_open(const void*,size_t,ayatsuri_model*);
size_t ayatsuri_stream_candidate_bytes(const ayatsuri_model *model);
ayatsuri_model_status ayatsuri_stream_candidate_init(const ayatsuri_model*,void*,size_t,const void*,uint32_t,ayatsuri_stream_candidate*);
ayatsuri_model_status ayatsuri_stream_candidate_targets(ayatsuri_stream_candidate*,const void*,uint32_t,const ayatsuri_model_targets*);
ayatsuri_model_status ayatsuri_stream_candidate_dispose(ayatsuri_stream_candidate*,const void*,uint32_t);
/* Dedicated pending snapshot only: disjoint from READY/pinned/DMA-owned storage.
 * Success only completes pending; it does not publish or pin. Failure may leave
 * private pending/candidate work partial, and MUST preserve the separate ready
 * owner. Caller supplies immutable source inputs and a 32-result provider slot.
 * Do not rebuild/reuse pending while a reader or display submission owns it.
 * Exclusive synchronous trusted-native owner only; not ISR/thread safe.
 * Provider callback restrictions are identical to ayatsuri_model_provider.h. */
ayatsuri_model_status ayatsuri_stream_candidate_frame(ayatsuri_stream_candidate*,const void*,uint32_t,
 const float camera[9],const float light[6],ayatsuri_model_geometry_provider,void*,
 ayatsuri_skin_result slot[32],ksn_mesh_snapshot *pending);
/* Explicit route-checked extension. Exact model identity and one ordered route
 * for EVERY mesh are required. OPAQUE retains the usual append; MATERIAL skips
 * that append to avoid duplicate opaque occlusion. Sink failure leaves private
 * pending geometry/payload partial, never published. Programs/source-package
 * hashes and per-polygon material provenance require separate admission. */
ayatsuri_model_status ayatsuri_stream_candidate_frame_attributed(ayatsuri_stream_candidate*,const void*,uint32_t,
 const float[9],const float[6],ayatsuri_model_geometry_provider,void*,ayatsuri_skin_result[32],
 ksn_mesh_snapshot*,const ayatsuri_stream_routes*,ayatsuri_stream_attributed_sink,void*);
/* Explicit unlit material-only capability. Caller must admit exact constant
 * opaque programs before publication. No lighting/normal validation is promised.
 * Existing APIs remain strict. Sink local.normal contains NaN sentinels and must
 * never be consumed; projected colors are zero. XYZ, topology and visibility
 * retain their ordinary meaning. All mesh routes must be MATERIAL, even hidden.
 * Typed provider writes object-local positions only; synchronous borrow rules
 * and pending rollback/ownership requirements above remain unchanged. */
typedef struct {float position[3];} ayatsuri_stream_position;
typedef bool (*ayatsuri_stream_position_provider)(void*,unsigned,unsigned,ayatsuri_stream_position*);
ayatsuri_model_status ayatsuri_stream_candidate_frame_attributed_unlit(ayatsuri_stream_candidate*,const void*,uint32_t,
 const float[9],const float[6],ayatsuri_stream_position_provider,void*,ayatsuri_skin_result[32],
 ksn_mesh_snapshot*,const ayatsuri_stream_routes*,ayatsuri_stream_attributed_sink,void*);
/* Z-up orthographic view for sources authored with +Z up. Use one winding
 * selector per material route, in route order. The span is borrowed until
 * return. The sink owns material admission and depth/alpha composition. */
ayatsuri_model_status ayatsuri_stream_candidate_frame_attributed_z_up(ayatsuri_stream_candidate*,const void*,uint32_t,
 const float[9],const float[6],ayatsuri_stream_position_provider,void*,ayatsuri_skin_result[32],
 ksn_mesh_snapshot*,const ayatsuri_stream_routes*,const ayatsuri_visibility_faces*,size_t,
 ayatsuri_stream_attributed_sink,void*);
#endif
