// Per-frame breakdown probe (docs/perf/frame-breakdown.md). Diagnostic:
// compiled to nothing unless CONFIG_POCKET_FRAME_BREAKDOWN.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "sdkconfig.h"

// Display pacing variants the probe rotates window by window (sec.6): OLD
// always yields one tick after an overrun, NOYIELD (the shipping rule) only
// when the UI core's idle task has not run for a while, DEADLINE also waits
// for an absolute deadline in microseconds instead of a period in ms.
enum { FRAME_BD_PACE_OLD, FRAME_BD_PACE_NOYIELD, FRAME_BD_PACE_DEADLINE };

#ifdef CONFIG_POCKET_FRAME_BREAKDOWN
void frame_bd_js(uint32_t us);
void frame_bd_present(uint32_t render_us, uint32_t send_us);
void frame_bd_tick(uint32_t us);
// The KASANE_PAINT/SHIKI_TURN print, timed around both lines.
void frame_bd_log(uint32_t us);
// Native-heap pressure as sample_memory_pressure() hands it to the block cache.
void frame_bd_pressure(bool on);
// End of one frame() period, from main.c: held_us is the loop iteration's
// work, period_us the whole period including its wait.
void frame_bd_period(uint32_t held_us, uint32_t period_us);
unsigned frame_bd_pace_mode(void);
// The block-cache pressure sample_memory_pressure() should apply: `on` as
// measured, or forced on/off by POCKET_FRAME_BD_PRESS_AB.
bool frame_bd_pressure_override(bool on);
// An overrun frame took the one-tick yield.
void frame_bd_yielded(void);
#else
static inline void frame_bd_js(uint32_t us) {(void)us;}
static inline void frame_bd_present(uint32_t r, uint32_t s) {(void)r;(void)s;}
static inline void frame_bd_tick(uint32_t us) {(void)us;}
static inline void frame_bd_log(uint32_t us) {(void)us;}
static inline void frame_bd_pressure(bool on) {(void)on;}
static inline void frame_bd_period(uint32_t h, uint32_t p) {(void)h;(void)p;}
static inline unsigned frame_bd_pace_mode(void) {return FRAME_BD_PACE_NOYIELD;}
static inline bool frame_bd_pressure_override(bool on) {return on;}
static inline void frame_bd_yielded(void) {}
#endif
