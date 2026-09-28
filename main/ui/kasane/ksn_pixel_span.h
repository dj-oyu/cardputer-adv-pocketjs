#ifndef KSN_PIXEL_SPAN_H
#define KSN_PIXEL_SPAN_H

#include "ksn_pixel_function.h"

/* Experimental instruction-major scalar backend. The image must already be
 * validated and bound. Evaluation owns its register planes on the caller's
 * stack (3,840 bytes maximum) and never changes the image's row cache. */
bool ksn_pixel_span_eval(const ksn_pixel_image *image, uint16_t y, uint16_t x,
                         uint16_t count, uint16_t *rgb565, uint8_t *alpha);

#endif
