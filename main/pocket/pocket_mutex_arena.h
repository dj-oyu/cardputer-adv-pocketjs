#pragma once

#include <stdbool.h>

/* Group mutexes created while one or more native producers are active.
 * Every successful activate must be paired with deactivate. The backing is
 * released after the last deactivate if no mutex still owns it. Allocation
 * failure does not enter a scope; FreeRTOS's ordinary allocator still works. */
#ifdef ESP_PLATFORM
bool pocket_mutex_arena_activate(void);
void pocket_mutex_arena_deactivate(void);

#ifdef KASANE_P0_PROBE
void pocket_mutex_arena_report(void);
/* Fresh-boot, home-only diagnostic: force two FreeRTOS tasks to race the
 * initial backing allocation, then verify shared lifetime and cleanup. */
bool pocket_mutex_arena_device_race_probe(void);
/* Inject one backing-allocation failure without exhausting the device heap. */
bool pocket_mutex_arena_device_oom_probe(void);
#endif
#else
static inline bool pocket_mutex_arena_activate(void) {return true;}
static inline void pocket_mutex_arena_deactivate(void) {}
#ifdef KASANE_P0_PROBE
static inline void pocket_mutex_arena_report(void) {}
#endif
#endif
