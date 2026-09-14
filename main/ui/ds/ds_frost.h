#ifndef DS_FROST_H
#define DS_FROST_H
#include "ds_types.h"
#define DS_FROST_BYTES 2048u
typedef union {
    struct {
        uint16_t image[30*17],line[30];
        uint16_t next_y;
        uint8_t phase;
    } state;
    uint8_t bytes[DS_FROST_BYTES];
} ds_frost;
#ifdef __cplusplus
extern "C" {
#endif
/* Pixel filter only; owner supplies immutable APP-only strips, in order.
 * No capture handle, attachment or implicit next-submission state here. */
void ds_frost_init(ds_frost *frost);
ds_result ds_frost_feed(ds_frost *frost,uint16_t y,uint16_t rows,const uint16_t *pixels);
ds_result ds_frost_blur(ds_frost *frost,uint8_t radius);
ds_result ds_frost_span(const ds_frost *frost,uint16_t y,uint16_t x,uint16_t count,
                        ds_rgba tint,uint16_t *pixels);
/* Scalar control for kernel A/B diagnostics; identical pixel contract. */
ds_result ds_frost_span_scalar(const ds_frost *frost,uint16_t y,uint16_t x,uint16_t count,
                               ds_rgba tint,uint16_t *pixels);
#ifdef __cplusplus
}
#endif
#endif
