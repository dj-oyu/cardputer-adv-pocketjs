#ifndef KSN_PORTS_H
#define KSN_PORTS_H
#include "ksn_types.h"
typedef struct {
    void *ctx;
    /* Absolute destination coordinates, at most 64 coverage bytes. count=0
     * validates availability before the first transfer (out may be NULL).
     * Immutable font data, no heap/I/O/JS or owner mutation in this callback.
     * reveal counts Unicode scalars. Text starts at bounds.x0/bounds.y0. */
    ksn_result (*span)(void *,const ksn_draw *,uint16_t reveal,int x,int y,
                       unsigned count,uint8_t *out);
} ksn_text_port;
typedef struct {
    void *ctx;
    uint16_t *(*strip)(void *);
    ksn_result (*present)(void *,uint16_t y,uint16_t rows,const uint16_t *pixels);
    uint16_t width,height,strip_rows;
    const ksn_text_port *text;
    /* Optional. Transfers columns [x,x+cols) of the strip's `rows` rows, reading
     * each row at `pixels + row*width + x`; the rest of the strip is NOT valid
     * and must not be sent. Supplying it is what lets the renderer composite a
     * band's damaged columns only, so an owner that can only push whole rows
     * leaves it NULL and gets the full-width behaviour it had. Deciding per
     * frame is allowed and is how a diagnostic that dumps whole rows opts out. */
    ksn_result (*present_rect)(void *,uint16_t x,uint16_t y,uint16_t cols,
                               uint16_t rows,const uint16_t *pixels);
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
