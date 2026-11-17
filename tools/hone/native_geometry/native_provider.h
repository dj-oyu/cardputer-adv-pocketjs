#ifndef HONE_NATIVE_PROVIDER_CANDIDATE_H
#define HONE_NATIVE_PROVIDER_CANDIDATE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Include the generated geometry, native_gather.h and
 * ayatsuri_model_provider.h first. The caller owns a completely prepared,
 * unpublished frame for every referenced mesh and an immutable node-to-part
 * map indexed by actual model node ID, including holes for non-mesh nodes.
 * A compact geometry-ordinal list is not such a map. All arrays are trusted,
 * complete and disjoint from slot. This only
 * supplies synchronous geometry; it does not evaluate or publish a model. */
typedef struct {
    const hone_input_mesh *mesh;
    const float *source_positions, *packed_normals;
} hone_provider_mesh;
typedef struct {
    uint32_t mesh_index, count;
    const uint32_t *prepared_vertices;
} hone_provider_part;
typedef struct {
    const hone_provider_mesh *meshes;
    size_t mesh_count;
    const hone_provider_part *nodes;
    size_t node_count;
} hone_provider_context;
static inline bool hone_native_provider(void *opaque,unsigned node_id,
                                        unsigned count,ayatsuri_skin_result *slot) {
    const hone_provider_context *ctx=opaque;
    if(!ctx||!ctx->meshes||!ctx->nodes||!slot||node_id>=ctx->node_count||count>32)return false;
    const hone_provider_part *part=&ctx->nodes[node_id];
    if(part->count!=count||part->mesh_index>=ctx->mesh_count)return false;
    const hone_provider_mesh *mesh=&ctx->meshes[part->mesh_index];
    hone_gather_index plan[32];
    if(!hone_gather_plan(mesh->mesh,part->prepared_vertices,count,
                         mesh->source_positions,mesh->packed_normals,plan))return false;
    for(unsigned i=0;i<count;i++)for(unsigned k=0;k<3;k++){
        slot[i].position[k]=mesh->source_positions[(size_t)plan[i].source*3+k];
        slot[i].normal[k]=mesh->packed_normals[(size_t)plan[i].normal*3+k];
    }
    return true;
}
#endif
