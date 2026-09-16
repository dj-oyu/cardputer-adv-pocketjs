#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "quickjs.h"
#include "ui/kasane/ksn_view_host.h"
#include "system/sys_notify.h"

/* QuickJS-facing APP lease into the native Kasane runtime. Allocation is lazy;
 * reading features costs no arena. Reset detaches APP and releases its wrapper
 * bookkeeping; a native SYSTEM owner keeps the shared storage alive.
 * Owner task only, reset outside guest/render callbacks. */
esp_err_t pocket_kasane_install(JSContext *ctx, void *user_data);
void pocket_kasane_reset(void);

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
