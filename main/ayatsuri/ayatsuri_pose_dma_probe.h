#ifndef AYATSURI_POSE_DMA_PROBE_H
#define AYATSURI_POSE_DMA_PROBE_H

#include <stdbool.h>
#include <stdint.h>

void ayatsuri_pose_dma_probe_step(uint32_t generation, bool restart, bool capture);

#endif
