#pragma once
#include <stdint.h>

// Parameters only. Stored in the existing releasable flower scene block.
typedef struct {
    int sun,phase,breath;
    unsigned seed;
} GardenFrame;
void garden_prepare(GardenFrame *frame,float time);
void garden_row(uint16_t *row,int y,const GardenFrame *frame);
