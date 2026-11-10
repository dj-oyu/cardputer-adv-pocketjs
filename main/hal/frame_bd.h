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

// P0 of docs/perf/two-core-pipeline.md: the first point in a JS turn where a
// two-core pipeline would have to join the display (js_pre is the turn up to
// it, js_post the rest), and why a frame could not have been overlapped.
enum { FRAME_BD_JOIN_SHIKI_COMMIT, FRAME_BD_JOIN_KASANE_TX, FRAME_BD_JOIN_SITES };
enum { FRAME_BD_NO_CONT=1u<<0, FRAME_BD_NO_CAPTURE=1u<<1, FRAME_BD_NO_TEXT=1u<<2,
       FRAME_BD_NO_NOTICE=1u<<3, FRAME_BD_NO_RESOURCE=1u<<4, FRAME_BD_NO_SOUND_CAPTURE=1u<<5 };

#ifdef CONFIG_POCKET_FRAME_BREAKDOWN
// Brackets the frame() turn (not continuations).
void frame_bd_turn_begin(void);
void frame_bd_turn_end(void);
void frame_bd_join_point(unsigned site);
// The present at the end of a frame() turn: `why` is the FRAME_BD_NO_* mask
// that would have kept it synchronous (0: it could overlap).
void frame_bd_eligible(unsigned why);
void frame_bd_js(uint32_t us);
void frame_bd_present(uint32_t render_us, uint32_t send_us);
void frame_bd_tick(uint32_t us);
// The KASANE_PAINT/SHIKI_TURN print, timed around both lines.
void frame_bd_log(uint32_t us);
// Native-heap pressure as sample_memory_pressure() hands it to the block cache,
// and the native sample that decided it when one was taken (native=true).
void frame_bd_pressure(bool on, bool native, uint32_t free_bytes, uint32_t largest);
// End of one frame() period, from main.c: held_us is the loop iteration's
// work, period_us the whole period including its wait.
void frame_bd_period(uint32_t held_us, uint32_t period_us);
unsigned frame_bd_pace_mode(void);
// P1 (POCKET_FRAME_BD_DISP_AB): whether this window presents on the display
// task; otherwise as CONFIG_POCKET_DISP_TASK says.
bool frame_bd_disp_on_task(void);
// One input_task poll (keyboard and IMU over I2C) took `us`, on CPU 0.
void frame_bd_input(uint32_t us);
// The block-cache pressure sample_memory_pressure() should apply: `on` as
// measured, or forced on/off by POCKET_FRAME_BD_PRESS_AB.
bool frame_bd_pressure_override(bool on);
// An overrun frame took the one-tick yield.
void frame_bd_yielded(void);
#else
static inline void frame_bd_turn_begin(void) {}
static inline void frame_bd_turn_end(void) {}
static inline void frame_bd_join_point(unsigned site) {(void)site;}
static inline void frame_bd_eligible(unsigned why) {(void)why;}
static inline void frame_bd_js(uint32_t us) {(void)us;}
static inline void frame_bd_present(uint32_t r, uint32_t s) {(void)r;(void)s;}
static inline void frame_bd_tick(uint32_t us) {(void)us;}
static inline void frame_bd_log(uint32_t us) {(void)us;}
static inline void frame_bd_pressure(bool on, bool n, uint32_t f, uint32_t l) {(void)on;(void)n;(void)f;(void)l;}
static inline void frame_bd_period(uint32_t h, uint32_t p) {(void)h;(void)p;}
static inline unsigned frame_bd_pace_mode(void) {return FRAME_BD_PACE_NOYIELD;}
static inline bool frame_bd_disp_on_task(void) {return true;}
static inline void frame_bd_input(uint32_t us) {(void)us;}
static inline bool frame_bd_pressure_override(bool on) {return on;}
static inline void frame_bd_yielded(void) {}
#endif
