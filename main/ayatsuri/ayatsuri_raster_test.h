#ifndef AYATSURI_RASTER_TEST_H
#define AYATSURI_RASTER_TEST_H

#include "ayatsuri_exact_damage.h"

enum { AY_RASTER_MARKER_WIDTH = 5 };

static inline int ay_raster_marker(unsigned frame)
{
    /* A constant speed in either direction. Keep the stripe clear of the HUD. */
    unsigned step = frame % 28;
    return 6 + 3 * (step <= 14 ? step : 28 - step);
}

static inline ay_damage_span ay_raster_marker_span(int x)
{
    if (x < 0) return (ay_damage_span){0, 0};
    return (ay_damage_span){x & ~15, (x + AY_RASTER_MARKER_WIDTH + 15) & ~15};
}

static inline void ay_raster_composite(uint16_t *model, const uint16_t *background,
                                       ay_damage_span window, int marker)
{
    /* Zero is the same empty model pixel used by the exact damage history. */
    for (unsigned x = window.left; x < window.right; ++x) {
        if (!model[x]) model[x] = background[x];
    }
    if (marker >= 0) {
        const int left = marker > window.left ? marker : window.left;
        const int end = marker + AY_RASTER_MARKER_WIDTH;
        const int right = end < window.right ? end : window.right;
        for (int x = left; x < right; ++x) {
            model[x] = x == marker || x == end - 1 ? 0 : UINT16_MAX;
        }
    }
}

#endif
