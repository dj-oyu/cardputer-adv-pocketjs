#ifndef KSN_PROC_GRID_RESIZE_H
#define KSN_PROC_GRID_RESIZE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Registered RGB565 bilinear resampler. Coordinate and weight tables are
 * fixed at registration; source pixels may change on every frame. */
typedef struct {
    uint16_t source_width, source_height, width, height;
    uint16_t x0[256], y0[256];
    uint8_t fx[256], fy[256]; /* fractions in units of 1/128 */
} ksn_grid_resize_plan;

bool ksn_grid_resize_prepare(ksn_grid_resize_plan *plan,
                             unsigned source_width, unsigned source_height,
                             unsigned width, unsigned height);
/* Span-backed mode has no whole-image buffer limit. */
bool ksn_grid_resize_prepare_stream(ksn_grid_resize_plan *plan,
                                    unsigned source_width, unsigned source_height,
                                    unsigned width, unsigned height);
bool ksn_grid_resize_pie_available(void);
#ifdef KASANE_PROC_DEVICE_PROBE
/* Diagnostic-only A/B controls; the shipping path always uses both changes. */
extern bool ksn_grid_resize_fast_weights;
extern bool ksn_grid_resize_shared_qr;
typedef struct {
    uint32_t flat, sparse, dense, dense_pie, dense_scalar, identity;
} ksn_grid_resize_routes;
void ksn_grid_resize_routes_read(ksn_grid_resize_routes *out);
#endif
bool ksn_grid_resize_span(const ksn_grid_resize_plan *plan,
                          unsigned y, unsigned x, unsigned count,
                          const uint16_t *row0, const uint16_t *row1,
                          uint16_t *dest, bool enable_pie, bool *used_pie);
bool ksn_grid_resize_run(const ksn_grid_resize_plan *plan,
                         const int16_t *source, size_t source_count,
                         int16_t *dest, size_t dest_count,
                         bool enable_pie, bool *used_pie);

#endif
