#include "aynm_mesh.h"
#include <string.h>

static bool release_mesh(void *context)
{
    hone_aynm_mesh *source = context;
    source->held = false;
    return true;
}

static bool render_mesh(void *context, hone_rect r, const hone_target *t, hone_span *span)
{
    hone_aynm_mesh *source = context;
    (void)span; /* The default conservative full span is valid. */
    for (unsigned y = 0; y < r.height; ++y) {
        uint16_t *dest = (uint16_t *)((uint8_t *)t->pixels + y * t->stride_bytes);
        uint16_t *row = r.width == KSN_PROC_W ? dest : source->row;
        for (unsigned x = 0; x < KSN_PROC_W; ++x) row[x] = source->background;
        if (!ksn_mesh_render(&source->mesh, row, r.y + y, 1, source->depth, 1)) return false;
        if (row != dest) memcpy(dest, row + r.x, r.width * sizeof *dest);
    }
    return true;
}

static hone_result model_status(ayatsuri_model_status status)
{
    if (status == AYATSURI_MODEL_STATE) return HONE_CLOSED;
    if (status == AYATSURI_MODEL_BUSY) return HONE_BUSY;
    if (status == AYATSURI_MODEL_ENDIAN) return HONE_UNSUPPORTED;
    return HONE_INVALID_ARGUMENT;
}

static hone_result append_mesh(ksn_mesh_snapshot *mesh, const ayatsuri_model_evaluation *e,
                               const ayatsuri_model_packet *packet)
{
    ayatsuri_model_status status;
    bool appended;
    if (packet->kind == 4) {
        ayatsuri_model_mesh_view v;
        status = ayatsuri_model_instance_mesh_view(e->instance, e->owner, e->generation, packet->node_id, &v);
        if (status != AYATSURI_MODEL_OK) return model_status(status);
        if (((const ksn_proc_gouraud_patch *)v.patch)->header.holes) return HONE_UNSUPPORTED;
        appended = ksn_mesh_append(mesh, v.patch, v.depth, v.node_id);
    } else if (packet->kind == 5) {
        ayatsuri_model_sided_mesh_view v;
        status = ayatsuri_model_instance_sided_mesh_view(e->instance, e->owner, e->generation, packet->node_id, &v);
        if (status != AYATSURI_MODEL_OK) return model_status(status);
        if (((const ksn_proc_gouraud_patch *)v.patch)->header.holes) return HONE_UNSUPPORTED;
        appended = ksn_mesh_append_sided(mesh, v.patch, v.depth, v.back_colors, v.back_faces, v.node_id);
    } else {
        return HONE_UNSUPPORTED;
    }
    return appended ? HONE_OK : HONE_LIMIT_EXCEEDED;
}

hone_result hone_frame_submit_aynm_mesh(hone_frame *f, const ayatsuri_model_evaluation *e,
        const float camera[9], const float light[6], const hone_rect *clip,
        hone_aynm_mesh *source, void *bytes, size_t capacity, uint32_t work_limit)
{
    if (!f || !e || !camera || !light || !source || !bytes || !capacity || !work_limit)
        return HONE_INVALID_ARGUMENT;
    if (source->held || f->busy) return HONE_BUSY;
    if (f->phase != HONE_BUILDING) return HONE_CLOSED;
    if (f->options.width != KSN_PROC_W || f->options.height != KSN_PROC_H) return HONE_UNSUPPORTED;
    ayatsuri_model_status status = ayatsuri_model_evaluation_project(e, camera, light);
    if (status != AYATSURI_MODEL_OK) return model_status(status);

    /* Only unpublished storage is touched until the entire sequence fits. */
    ksn_mesh_snapshot candidate = {.packets = {bytes, capacity, 0, 0},
                                   .work_limit = work_limit, .enabled = true};
    uint32_t cursor = 0;
    ayatsuri_model_packet packet;
    while ((status = ayatsuri_model_instance_next(e->instance, e->owner, e->generation,
                                                   &cursor, &packet)) == AYATSURI_MODEL_OK) {
        hone_result result = append_mesh(&candidate, e, &packet);
        if (result != HONE_OK) return result;
    }
    if (status != AYATSURI_MODEL_DONE) return model_status(status);
    hone_drawable drawable = {source, render_mesh, release_mesh};
    hone_result result = hone_frame_submit(f, &drawable, clip);
    if (result == HONE_OK) {
        source->mesh = candidate;
        source->background = f->options.clear;
        source->held = true;
    }
    return result;
}
