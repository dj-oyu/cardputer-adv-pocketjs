#ifndef POCKET_PROC_H
#define POCKET_PROC_H
#include <stdbool.h>
#include "quickjs.h"
#include "esp_err.h"
#include "ui/kasane/ksn_ports.h"
#include "ui/kasane/ksn_proc_plan.h"

/* A small APP-session adapter for registered procedural IR. */
esp_err_t pocket_proc_install(JSContext *ctx, JSValueConst kasane_namespace);
void pocket_proc_reset(void);
/* The built-in plans register('name', args) looks up (docs/kasane/flash-plan.md):
 * a const table the firmware links, generated from the apps' @plan functions.
 * The table must outlive every session; reset() keeps it. Without a call,
 * or with count 0, every name is unknown (INVALID_ARGUMENT). */
void pocket_proc_rom_plans(const ksn_proc_rom_entry *entries,unsigned count);
void pocket_proc_end_turn(void);
/* One draw() for a native caller (pocket_derby.c), with the inputs it would
 * have put in the JS array: the same checks in the same order (beginFrame,
 * reentrancy, stale handle, finite and float-finite inputs, the VM and frame
 * limits) and the same PocketError. Returns JS_UNDEFINED or JS_EXCEPTION. */
JSValue pocket_proc_draw_numbers(JSContext *ctx,uint32_t handle,const double *in,unsigned n);
#ifdef POCKET_PROC_DRAW_HOOK
/* Host harness only: called after each native draw that succeeded, with the
 * inputs as given; with pocket_proc_hook_dry set, the draw is not run at all
 * (the harness compares input lists without frames or plans). */
void pocket_proc_draw_hook(uint32_t handle,const double *in,unsigned n);
extern bool pocket_proc_hook_dry;
#endif
bool pocket_proc_has_frame(void);
bool pocket_proc_pending(void);
bool pocket_proc_backdrop_pending(void);
/* Session-scoped dynamic image. Its descriptor borrows the adapter until reset. */
void pocket_proc_image_port(ksn_image_port *out);
void pocket_proc_image_port_at(ksn_image_port *out,unsigned surface);
void pocket_proc_image_mode(void);
void pocket_proc_image_mode_at(unsigned surface);
bool pocket_proc_is_image_mode(void);
ksn_result pocket_proc_backdrop(void *ctx,uint16_t y,uint16_t rows,uint16_t *pixels);
/* Called only after the native presenter attempts a pending procedural frame. */
void pocket_proc_present_result(ksn_result result);
/* Native FLOWER backdrop presentation can ACK only procedural image nodes. */
void pocket_proc_overlay_present_result(ksn_result result);
void pocket_proc_batch_counts(uint32_t *scalar,uint32_t *pie);
#ifdef KASANE_MEGADEMO_TRACE
/* Per-turn costs for the MEGADEMO trace (app_session.c), read and cleared
 * once per host turn. Times are esp_timer microseconds except band_cy. */
typedef struct {
    uint32_t reg_n,reg_us,prep_us,prep_max_us,unreg_n;
    uint32_t draw_n,draw_us,draw_max_us,band_n,band_cy;
    /* commit(), replace()/patch(), and panel presents made while a commit
     * had no view update after it yet: the picture that pairs a new image
     * with the previous frame's view tree. */
    uint32_t commit_n,view_n,split_n;
} pocket_proc_trace;
void pocket_proc_trace_take(pocket_proc_trace *out);
void pocket_proc_trace_view(void);
void pocket_proc_trace_presented(void);
#ifdef KASANE_BGCOST_TRACE
/* Background-cost study (docs/kasane/derby-background-cost.md). Per turn: the
 * VM steps of every draw and the geometry of the frame the bands last read --
 * segments, segment-band intersections (the scan cost's partner) and the
 * Bresenham iterations the bands walk (the paint cost's partner). Computed
 * between turns, outside every timed span. Per plan: draws, microseconds,
 * steps and segments summed over a window and printed as one BGP line. */
typedef struct { uint32_t steps,seg,ras,hits,walk,commit_us,view_us; } pocket_proc_bgcost;
void pocket_proc_bgcost_take(pocket_proc_bgcost *out);
void pocket_proc_bgcost_plans_emit(void);
/* kasane.patch() wall time, its callback (the HUD) included. */
void pocket_proc_bgcost_view_us(uint32_t us);
#endif
#endif
#ifdef KASANE_PROC_JS_DIAGNOSTIC
void pocket_proc_image_prof_read(uint32_t *band_count,uint32_t *band_cycles,
                                 uint32_t *span_count,uint32_t *span_cycles);
#endif
#endif
