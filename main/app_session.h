#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
esp_err_t app_start(void);
// Run a source the caller owns; the bytes must outlive the start, and only the
// start -- the guest parses straight out of them and keeps nothing afterwards.
//
// `prelude` is evaluated first and in the same realm, so its top-level
// declarations are visible to `source` while the two keep their own line
// numbering in an error. NULL when there is none.
esp_err_t app_start_source(const char *prelude, size_t prelude_length,
                           const char *source, size_t length);
esp_err_t app_start_test(char test);
void app_force_redraw(void);
esp_err_t app_tick(uint32_t buttons);
void app_stop(void);
void app_request_stop(void);
void app_report(void);
// The last exception a Playground run reported, or "" when it ran clean.
const char *app_error(void);
