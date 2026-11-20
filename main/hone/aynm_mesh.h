#ifndef HONE_AYNM_MESH_H
#define HONE_AYNM_MESH_H
#include "hone_frame.h"
#include "ayatsuri/ayatsuri_model.h"
#include "ui/kasane/ksn_mesh_depth.h"

/* One owned projected AYNM input. Its meshes share depth, with the existing
 * compact renderer's first-face tie rule. It paints an opaque rectangle over
 * earlier Hone inputs. Analytic shapes and mesh holes are unsupported.
 * Zero-initialize this object. It and the exclusive packet arena must stay
 * disjoint from all model, frame, target and other live source storage until
 * the frame releases them. They can then be reused for another evaluation. */
typedef struct {
    ksn_mesh_snapshot mesh;
    float depth[KSN_PROC_W];
    uint16_t row[KSN_PROC_W], background;
    bool held;
} hone_aynm_mesh;
hone_result hone_frame_submit_aynm_mesh(hone_frame *, const ayatsuri_model_evaluation *,
        const float camera[9], const float light[6], const hone_rect *clip,
        hone_aynm_mesh *, void *packet_bytes, size_t capacity, uint32_t mesh_work_limit);
#endif
