#ifndef HONE_COMPANION_MORPH_H
#define HONE_COMPANION_MORPH_H

#include <stdint.h>
#include <string.h>

enum { HONE_COMPANION_AXES = 7 };

typedef struct {
    uint16_t vertex;
    uint8_t axis;
    float delta[3];
} hone_companion_delta;

typedef struct {
    uint16_t vertices, count;
    uint8_t axes;
    uint32_t fingerprint;
    const hone_companion_delta *deltas;
} hone_companion_morph;

static inline uint32_t hone_companion_position_hash(const float *positions, unsigned vertices)
{
    uint32_t hash = UINT32_C(2166136261);
    for (unsigned i = 0; i < 3 * vertices; ++i) {
        uint32_t bits;
        memcpy(&bits, positions + i, sizeof bits);
        hash = (hash ^ bits) * UINT32_C(16777619);
    }
    return hash;
}

static inline unsigned hone_companion_changed_axes(const float before[HONE_COMPANION_AXES],
                                                    const float after[HONE_COMPANION_AXES])
{
    unsigned changed = 0;
    for (unsigned i = 0; i < HONE_COMPANION_AXES; ++i)
        if (before[i] != after[i]) changed |= 1u << i;
    return changed;
}

/* Admission verifies the source fingerprint, axis mask and every delta.
 * This operates on the caller's temporary copy, before morphing/skinning. */
static inline void hone_companion_apply(const hone_companion_morph *morph,
                                         const float controls[HONE_COMPANION_AXES], float *positions)
{
    for (unsigned i = 0; i < morph->count; ++i) {
        const hone_companion_delta *delta = morph->deltas + i;
        const float weight = controls[delta->axis];
        float *p = positions + 3 * delta->vertex;
        for (unsigned k = 0; k < 3; ++k)
            p[k] += weight * delta->delta[k];
    }
}

#endif
