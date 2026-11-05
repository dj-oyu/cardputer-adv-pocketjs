#ifndef AYATSURI_MODEL_PROVIDER_H
#define AYATSURI_MODEL_PROVIDER_H
#include "ayatsuri_model_skin.h"
/* Experimental trusted-native producer; no JS registration or wire extension.
 * One reusable slot holds up to 32 node-local position/normal results.
 * Positions must be finite within [-4096,4096]; normals must be finite with
 * squared length within 1e-4 of one, matching the existing geometry bridge.
 * Mesh nodes are visited, in model order, including disabled nodes; subset/cold
 * selection and arbitrary external callbacks are deliberately not supported.
 * Source topology, materials and hierarchy remain cold; node TRS applies once.
 * Provider may write the slot and its own private work/counters only. It must
 * not mutate candidate/model, camera/light, source inputs or ready snapshots,
 * nor retain the slot. No callback with script/user-code execution is allowed.
 * Context and immutable source inputs remain valid for the synchronous call.
 */
typedef bool (*ayatsuri_model_geometry_provider)(void *context,unsigned node_id,
        unsigned vertex_count,ayatsuri_skin_result *slot);
#ifdef AYATSURI_MODEL_EXPERIMENTAL_PROVIDER
/* Dedicated candidate only: this is NOT a ready-actor atomic update API.
 * Caller supplies an aligned, valid, writable 32-result slot disjoint from
 * candidate/model/arena/cold data/camera/light and externally ready storage.
 * Entry rejects invalid owner/generation/busy, storage and unsupported mesh
 * metadata before evaluation. Once evaluation starts, provider/geometry/numeric
 * failure leaves candidate not-ready, with private partial work/stats allowed.
 * Every accepted result is consumed before the slot is reused. No allocation.
 * A successful candidate can be copied into a frame-owned pending snapshot.
 * Only the caller's complete-frame commit may publish it; failed append must
 * retain the old ready frame. Pin/ACK lifetime and startup OOM belong to caller.
 * Caller enforces exclusive synchronous ownership; not thread/ISR safe.
 * Existing evaluate/evaluate_meshes failure contracts are unchanged.
 */
ayatsuri_model_status ayatsuri_model_candidate_evaluate_provider(
        ayatsuri_model_instance *candidate,const void *owner,uint32_t generation,
        const float camera[9],const float light[6],
        ayatsuri_model_geometry_provider provider,void *context,
        ayatsuri_skin_result *slot);
#endif
#endif
