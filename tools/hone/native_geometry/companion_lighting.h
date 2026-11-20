#ifndef HONE_COMPANION_LIGHTING_H
#define HONE_COMPANION_LIGHTING_H

#include <stdint.h>

/* Direction is unit length in the source mesh frame and scaled to the
 * 16-level palette and signed-byte normals. */
static inline unsigned hone_companion_light_level(const float direction[3],
                                                   const int8_t normal[3])
{
    const float lit = 0.5f + direction[0] * normal[0] +
                      direction[1] * normal[1] + direction[2] * normal[2];
    if (lit <= 0.0f) return 0;
    if (lit >= 15.0f) return 15;
    return (unsigned)lit;
}

#endif
