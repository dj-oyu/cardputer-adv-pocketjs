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
