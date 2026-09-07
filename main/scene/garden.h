#pragma once
#include <stdint.h>

// Parameters only. Stored in the existing releasable flower scene block.
typedef struct {
    int sun,phase,breath;
    unsigned seed;
} GardenFrame;
void garden_prepare(GardenFrame *frame,float time);
void garden_row(uint16_t *row,int y,const GardenFrame *frame);
#ifdef ESP_PLATFORM
// TEMPORARY, and it goes with flower.c's counters. garden_row is now made of
// two very different things -- three vector passes over the 240 pixels, then
// trunks, canopy and grass still scalar -- and flower.c's SPLIT line cannot
// tell them apart, which is exactly the number needed to decide what to do
// next. Reads the cycles spent in the vector half and clears the count.
uint32_t garden_prof_pixels(void);
#endif
