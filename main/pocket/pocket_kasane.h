#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "quickjs.h"
#include "ui/kasane/ksn_view_host.h"
#include "system/sys_notify.h"

/* QuickJS-facing APP lease into the native Kasane runtime. Allocation is lazy
 * (first Kasane call, or pocket_kasane_prepare); reading features costs no arena. Reset detaches APP and releases its wrapper
 * bookkeeping; a native SYSTEM owner keeps the shared storage alive.
 * Owner task only, reset outside guest/render callbacks. */
esp_err_t pocket_kasane_install(JSContext *ctx, void *user_data);
void pocket_kasane_reset(void);
/* Takes the native arena now, as one block, instead of at the guest's first
 * Kasane call. The session calls it before evaluating a source that names
 * kasane, while the heap is still unbroken. Failure is silent: the first call
 * then retries and reports OUT_OF_MEMORY where the app can see it. */
void pocket_kasane_prepare(void);

bool pocket_kasane_active(void);
ksn_result pocket_kasane_update_notice(const sys_notice *,uint16_t variant);
bool pocket_kasane_notice_composited(void);
bool pocket_kasane_system_pending(void);
bool pocket_kasane_has_submission(void);
ksn_result pocket_kasane_advance(uint64_t now_us);
bool pocket_kasane_animation_pending(void);
void pocket_kasane_animations_presented(uint64_t now_us);
void pocket_kasane_set_animation_time(uint64_t now_us);
bool pocket_kasane_needs_present(void);
void pocket_kasane_invalidate(void);
ksn_result pocket_kasane_present(const ksn_display_port *display,
                                 ksn_render_stats *stats);
void pocket_kasane_end_turn(void);
ksn_input_scope pocket_kasane_input_scope(bool host_priority);
