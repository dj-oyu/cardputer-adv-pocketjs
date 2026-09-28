#ifndef POCKET_PROC_H
#define POCKET_PROC_H
#include <stdbool.h>
#include "quickjs.h"
#include "esp_err.h"
#include "ui/kasane/ksn_ports.h"

/* A small APP-session adapter for registered procedural IR. */
esp_err_t pocket_proc_install(JSContext *ctx, JSValueConst kasane_namespace);
void pocket_proc_reset(void);
void pocket_proc_end_turn(void);
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
} pocket_proc_trace;
void pocket_proc_trace_take(pocket_proc_trace *out);
#endif
#ifdef KASANE_PROC_JS_DIAGNOSTIC
void pocket_proc_image_prof_read(uint32_t *band_count,uint32_t *band_cycles,
                                 uint32_t *span_count,uint32_t *span_cycles);
#endif
#endif
