#ifndef AYATSURI_MODEL_SKIN_H
#define AYATSURI_MODEL_SKIN_H
#include "ayatsuri_model.h"
#include "ayatsuri_skin.h"

/* C-only transient geometry bridge; no new AYNM wire section or JS capability.
 * Skin results are in the selected mesh NODE'S LOCAL space. Its hierarchy/TRS
 * is applied afterwards, exactly once. Topology, mask and material stay cold.
 * Only single-sided, hole-free mesh nodes are supported in this first slice.
 * Every vertex must be finite, position components within [-4096,4096], and
 * normal squared length within 1e-4 of 1 (the cold-mesh normal contract).
 * Count must match the original vertex count; duplicate node IDs reject.
 *
 * Descriptors and vertices are synchronous immutable borrows; nothing is
 * retained. They must be aligned, valid complete spans and disjoint from the
 * instance and its arena. Inputs may share storage with each other. Callers
 * own their palette, input weights and skin-result work; no instance/arena
 * allocation is added. Run ayatsuri_skin_evaluate before this call.
 *
 * Invalid geometry leaves ready packets and all instance stats unchanged.
 * Once evaluation starts, camera/projection/numeric failures invalidate the
 * whole instance, following the existing evaluate contract. Successful calls
 * replace current packets; copy them into a frame-owned snapshot before any
 * mutation/disposal. A subsequent ordinary evaluate restores cold geometry,
 * even with identical camera/light. An empty override batch is ordinary
 * evaluation. Nonempty batches always recompute; no hidden pointer/epoch cache.
 * This API does not own submission, commit, transport retry, or presentation. */
typedef struct {
    uint16_t node_id, vertex_count;
    const ayatsuri_skin_result *vertices;
} ayatsuri_model_mesh_geometry;

ayatsuri_model_status ayatsuri_model_instance_evaluate_meshes(
        ayatsuri_model_instance *instance,const void *owner,uint32_t generation,
        const float camera[9],const float light[6],
        const ayatsuri_model_mesh_geometry *meshes,size_t count);
#endif
