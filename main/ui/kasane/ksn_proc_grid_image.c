#include "ksn_proc_grid_image.h"

#include <string.h>

static ksn_result grid_span(void *ctx, uint16_t variant, uint16_t frame,
                            uint16_t y, uint16_t x, uint16_t count,
                            uint16_t *rgb565, uint8_t *alpha)
{
    const ksn_grid_image *image = ctx;
    if (!image || !image->ready || variant || frame ||
        y >= image->execution.shape.height ||
        x > image->execution.shape.width ||
        count > image->execution.shape.width - x ||
        (count && (!rgb565 || !alpha))) return KSN_INVALID;
    if (count) {
        const uint16_t *pixels = (const uint16_t *)
            image->execution.binding.data[KSN_GRID_DEST];
        size_t offset = (size_t)y * image->execution.shape.width + x;
        memcpy(rgb565, pixels + offset, (size_t)count * sizeof *rgb565);
        memset(alpha, 255, count);
    }
    return KSN_OK;
}

ksn_grid_status ksn_grid_image_bind(ksn_grid_image *image,
                                    const ksn_grid_plan *plan,
                                    const ksn_grid_shape *shape,
                                    const ksn_grid_binding *binding,
                                    ksn_grid_pie_policy policy)
{
    if (!image) return KSN_GRID_BAD_IR;
    *image = (ksn_grid_image){0};
    ksn_grid_status status = ksn_grid_begin(plan, shape, binding,
                                            &image->execution);
    if (status != KSN_GRID_OK) return status;
    const int64_t *out = image->execution.output;
    if (out[0] != 0 || out[1] != 1 ||
        out[2] != shape->width || out[3] || out[4]) {
        image->execution.safe = false;
        return KSN_GRID_BAD_INDEX;
    }
    image->policy = policy;
    return KSN_GRID_OK;
}

ksn_grid_status ksn_grid_image_run(ksn_grid_image *image)
{
    if (!image || !image->execution.safe) return KSN_GRID_BAD_IR;
    image->ready = false;
    ksn_grid_status status = ksn_grid_run_auto(&image->execution,
                                               &image->policy);
    if (status == KSN_GRID_OK) image->ready = true;
    return status;
}

bool ksn_grid_image_port(ksn_grid_image *image, ksn_image_port *out)
{
    if (!image || !out || !image->ready) return false;
    *out = (ksn_image_port){
        .ctx = image,
        .width = image->execution.shape.width,
        .height = image->execution.shape.height,
        .variants = 1,
        .frames = 1,
        .read_span = grid_span,
        .opaque = true
    };
    return true;
}
