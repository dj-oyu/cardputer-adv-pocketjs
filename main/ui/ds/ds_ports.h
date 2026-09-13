#ifndef DS_PORTS_H
#define DS_PORTS_H
#include "ds_types.h"
typedef struct {
    void *ctx;
    uint16_t *(*strip)(void *);
    ds_result (*present)(void *,uint16_t y,uint16_t rows,const uint16_t *pixels);
    uint16_t width,height,strip_rows;
} ds_display_port;
/* Borrow the existing board strip. No second framebuffer. */
typedef struct {
    void *ctx;
    uint16_t width,height,variants,frames;
    ds_result (*read_span)(void *,uint16_t variant,uint16_t frame,
                          uint16_t y,uint16_t x,uint16_t count,
                          uint16_t *rgb565,uint8_t *alpha);
} ds_image_port;
/* Source spans, not destination spans. Compositor resolves crop/scale.
 * Provider fills outputs without heap/I/O. Pet/PPT2 types stay in its adapter.
 * Register copies the descriptor; ctx and flash live until host.reset. */
typedef struct {
    ds_result (*register_image)(void *,const ds_image_port *,ds_resource *);
} ds_resource_api;
typedef enum { DS_BACKDROP_SOLID,DS_BACKDROP_FROSTED } ds_backdrop_mode;
typedef struct {
    uint8_t downsample,blur_radius;
    ds_rgba tint,fallback;
    uint32_t max_bytes;
} ds_backdrop_request;
typedef struct {
    ds_result (*capture)(void *,const ds_backdrop_request *,ds_backdrop_mode *actual);
    ds_result (*release)(void *);
} ds_backdrop_api;
/* Optional synchronous prototype: capture APP only, excluding SYSTEM.
 * Next APP replace uses this backdrop. OOM/unavailable -> OK + SOLID fallback.
 * BUSY means unchanged. release is BUSY while displayed/submitted state uses
 * capture. Close reconstructs/presents app before release. Incremental capture
 * and cancellation remain an explicit design question, not a working feature. */
#endif
