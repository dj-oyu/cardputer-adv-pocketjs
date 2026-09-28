#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "flower.h"
#include "glass_rain.h"

/* Diagnostic image only. Start immediately before one FLOWER candidate
 * allocation, after discarding its old spare slot. Keep the reservation only
 * through that allocation and its committed-frame fallback present. Call end
 * as soon as that present has completed, whether it succeeded or failed. */
bool flower_d1_pressure_begin(size_t candidate_block_bytes);
void flower_d1_pressure_end(void);
/* Call after flower_prepare. Equals flower_frame_capture's realloc request for
 * the current derived part count, including its alignment slack. */
size_t flower_d1_candidate_allocation_bytes(void);
/* Hash the exact FLOWER+rain backdrop that the shell repaints in 8-row bands.
 * Compare before pressure, during committed fallback, and after repair. */
uint32_t flower_d1_committed_hash(const flower_frame *frame,
                                  const glass_rain_frame *rain);
