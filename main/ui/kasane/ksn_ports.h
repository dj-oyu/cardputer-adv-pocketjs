#ifndef KSN_PORTS_H
#define KSN_PORTS_H
#include "ksn_types.h"
typedef struct {
    void *ctx;
    uint16_t *(*strip)(void *);
    ksn_result (*present)(void *,uint16_t y,uint16_t rows,const uint16_t *pixels);
    uint16_t width,height,strip_rows;
} ksn_display_port;
/* Borrow the existing board strip. No second framebuffer. */
typedef struct {
    void *ctx;
    uint16_t width,height,variants,frames;
    ksn_result (*read_span)(void *,uint16_t variant,uint16_t frame,
                          uint16_t y,uint16_t x,uint16_t count,
                          uint16_t *rgb565,uint8_t *alpha);
} ksn_image_port;
/* Source spans, not destination spans. Compositor resolves crop/scale.
 * Provider fills outputs without heap/I/O. Pet/PPT2 types stay in its adapter.
 * Register copies the descriptor; ctx and flash live until host.reset. */
typedef struct {
    ksn_result (*register_image)(void *,const ksn_image_port *,ksn_resource *);
} ksn_resource_api;
typedef enum { KSN_BACKDROP_SOLID,KSN_BACKDROP_FROSTED } ksn_backdrop_mode;
typedef struct {
    uint8_t downsample,blur_radius;
    ksn_rgba tint,fallback;
    uint32_t max_bytes;
} ksn_backdrop_request;
typedef struct {
    ksn_result (*capture)(void *,const ksn_backdrop_request *,ksn_backdrop_mode *actual);
    ksn_result (*release)(void *);
} ksn_backdrop_api;
/* Optional synchronous prototype: capture APP only, excluding SYSTEM.
 * Next APP replace uses this backdrop. OOM/unavailable -> OK + SOLID fallback.
 * BUSY means unchanged. release is BUSY while displayed/submitted state uses
 * capture. Close reconstructs/presents app before release. Incremental capture
 * and cancellation remain an explicit design question, not a working feature. */
#endif
