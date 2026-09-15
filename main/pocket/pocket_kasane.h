#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "quickjs.h"
#include "ui/kasane/ksn_view_host.h"

/* QuickJS-facing APP endpoint for the Kasane design system. Native storage is
 * allocated lazily on the first mutating call; reading features costs no Kasane
 * arena. All functions run on the JS owner task. */
esp_err_t pocket_kasane_install(JSContext *ctx, void *user_data);
void pocket_kasane_reset(void);

bool pocket_kasane_active(void);
bool pocket_kasane_has_submission(void);
bool pocket_kasane_needs_present(void);
void pocket_kasane_invalidate(void);
ksn_result pocket_kasane_present(const ksn_display_port *display,
                                 ksn_render_stats *stats);
void pocket_kasane_end_turn(void);
ksn_input_scope pocket_kasane_input_scope(bool host_priority);
