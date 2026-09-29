#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "quickjs.h"

/* Node-independent action service. Owner task only; install before input.text
 * contributes to the lazy namespace. Reset before destroying the JS realm. */
esp_err_t pocket_input_install(JSContext *,void *);
/* One forwarded button snapshot per guest turn, after pending jobs drain.
 * held() is updated even with no listeners or before the first namespace read. */
void pocket_input_pump(uint32_t buttons);
void pocket_input_reset(void);
/* The host has the keyboard this turn and the guest is not pumped (a picker
 * screen over the app): input.keys hides whatever is down now until it comes
 * up, and reports no edge from the gap. Turns the guest IS pumped through
 * (a text field, a blocking notice) are detected by the pump itself. */
void pocket_input_keys_withhold(void);
// Resident suspension (docs/vm/app-suspend-design.md sec.4).
void pocket_input_suspend(void);
