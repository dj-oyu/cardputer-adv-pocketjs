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
/* Session-scoped dynamic image. Its descriptor borrows the adapter until reset. */
void pocket_proc_image_port(ksn_image_port *out);
void pocket_proc_image_mode(void);
bool pocket_proc_is_image_mode(void);
ksn_result pocket_proc_backdrop(void *ctx,uint16_t y,uint16_t rows,uint16_t *pixels);
/* Called only after the native presenter attempts a pending procedural frame. */
void pocket_proc_present_result(ksn_result result);
void pocket_proc_batch_counts(uint32_t *scalar,uint32_t *pie);
#endif
