#ifndef AYATSURI_POSE_DMA_PROBE_H
#define AYATSURI_POSE_DMA_PROBE_H

#include <stdbool.h>
#include <stdint.h>

void ayatsuri_pose_dma_probe_step(uint32_t generation, bool restart, bool capture);
/* The UI owner must confirm its display drain before releasing the live model. */
bool ayatsuri_pose_dma_probe_close_after_drain(void);

#endif
