#ifndef AYATSURI_POSE_DMA_PROBE_H
#define AYATSURI_POSE_DMA_PROBE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    AY_COMPANION_NONE,
    AY_COMPANION_PREVIOUS_POSE,
    AY_COMPANION_NEXT_POSE,
    AY_COMPANION_PREVIOUS_EXPRESSION,
    AY_COMPANION_NEXT_EXPRESSION,
    AY_COMPANION_PLAY,
    AY_COMPANION_BENCHMARK,
    AY_COMPANION_RASTER_TOGGLE,
} ay_companion_command;

/* A positive deadline requests a UI turn for free-running Companion playback. */
int64_t ayatsuri_pose_dma_probe_step(uint32_t generation, ay_companion_command command, bool capture);
/* The UI owner must confirm its display drain before releasing the live model. */
bool ayatsuri_pose_dma_probe_close_after_drain(void);

#endif
