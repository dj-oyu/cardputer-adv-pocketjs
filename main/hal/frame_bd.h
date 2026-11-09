// Per-frame breakdown probe (docs/perf/60fps-roadmap.md). Diagnostic:
// compiled to nothing unless CONFIG_POCKET_FRAME_BREAKDOWN.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "sdkconfig.h"

#ifdef CONFIG_POCKET_FRAME_BREAKDOWN
void frame_bd_js(uint32_t us);
void frame_bd_present(uint32_t render_us, uint32_t send_us);
void frame_bd_tick(uint32_t us);
// End of one frame() period, from main.c: held_us is the loop iteration's
// work, period_us the whole period including its wait.
void frame_bd_period(uint32_t held_us, uint32_t period_us);
#else
static inline void frame_bd_js(uint32_t us) {(void)us;}
static inline void frame_bd_present(uint32_t r, uint32_t s) {(void)r;(void)s;}
static inline void frame_bd_tick(uint32_t us) {(void)us;}
static inline void frame_bd_period(uint32_t h, uint32_t p) {(void)h;(void)p;}
#endif
