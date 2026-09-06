#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "quickjs.h"

// Output a running script produced, kept where the host can draw it. quickjs's
// own print() writes to stdout, which reaches the USB log but never the screen,
// so the Playground installs its own and keeps the last few lines.
#define JSC_LINES 6
#define JSC_COLS  46

void jsconsole_clear(void);
unsigned jsconsole_count(void);
const char *jsconsole_line(unsigned i);   // 0 is the oldest kept line

// Registers print, console.log/error and __pjs_error on the realm. Pass to
// pocketjs_guest_quickjs_install_once().
esp_err_t jsconsole_install(JSContext *ctx, void *user_data);

// The last error a script reported, or NULL. Owned here, bounded.
const char *jsconsole_error(void);
void jsconsole_set_error(const char *text);
