#ifndef KSN_PROC_GRID_IMAGE_H
#define KSN_PROC_GRID_IMAGE_H

#include "ksn_proc_grid_pie.h"
#include "ksn_ports.h"

/* Borrowed, tightly packed RGB565 surface produced by a typed grid plan.
 * The owner keeps plan and bound buffers alive and does not write DEST while
 * Kasane may be reading the resource. No pixel copy or second framebuffer. */
typedef struct {
    ksn_grid_execution execution;
    ksn_grid_pie_policy policy;
    bool ready;
} ksn_grid_image;

ksn_grid_status ksn_grid_image_bind(ksn_grid_image *image,
                                    const ksn_grid_plan *plan,
                                    const ksn_grid_shape *shape,
                                    const ksn_grid_binding *binding,
                                    ksn_grid_pie_policy policy);
ksn_grid_status ksn_grid_image_run(ksn_grid_image *image);
bool ksn_grid_image_port(ksn_grid_image *image, ksn_image_port *out);

#endif
