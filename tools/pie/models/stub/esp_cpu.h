/* Host stand-in for ESP-IDF esp_cpu.h: the cycle counter used for profiling in main/scene/render_accel.c. */
#pragma once
#include <stdint.h>
static inline uint32_t esp_cpu_get_cycle_count(void) { return 0; }
