#ifndef AYATSURI_POSE_DMA_PROBE_H
#define AYATSURI_POSE_DMA_PROBE_H

#include <stdbool.h>
#include <stdint.h>

/* A positive deadline requests a UI turn for free-running Companion playback. */
int64_t ayatsuri_pose_dma_probe_step(uint32_t generation, bool restart, int inspect_pose, bool capture);
/* The UI owner must confirm its display drain before releasing the live model. */
bool ayatsuri_pose_dma_probe_close_after_drain(void);

#endif
