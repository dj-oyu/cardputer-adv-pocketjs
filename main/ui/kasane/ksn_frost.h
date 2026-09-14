#ifndef KSN_FROST_H
#define KSN_FROST_H
#include "ksn_types.h"
#define KSN_FROST_BYTES 2048u
typedef union {
    struct {
        uint16_t image[30*17],line[30];
        uint16_t next_y;
        uint8_t phase;
    } state;
    uint8_t bytes[KSN_FROST_BYTES];
} ksn_frost;
#ifdef __cplusplus
extern "C" {
#endif
/* Pixel filter only; owner supplies immutable APP-only strips, in order.
 * No capture handle, attachment or implicit next-submission state here. */
void ksn_frost_init(ksn_frost *frost);
ksn_result ksn_frost_feed(ksn_frost *frost,uint16_t y,uint16_t rows,const uint16_t *pixels);
ksn_result ksn_frost_blur(ksn_frost *frost,uint8_t radius);
ksn_result ksn_frost_span(const ksn_frost *frost,uint16_t y,uint16_t x,uint16_t count,
                        ksn_rgba tint,uint16_t *pixels);
/* Scalar control for kernel A/B diagnostics; identical pixel contract. */
ksn_result ksn_frost_span_scalar(const ksn_frost *frost,uint16_t y,uint16_t x,uint16_t count,
                               ksn_rgba tint,uint16_t *pixels);
#ifdef __cplusplus
}
#endif
#endif
