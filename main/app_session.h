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

// docs/common-api.md 3.1: a session the HOME SCREEN owns, running over the
// background rather than instead of it. It gets no Rust UI core, no font atlas
// and no rgb565 renderer -- see pocket_overlay.h -- and a guest heap sized for
// what is left while a scene is drawing rather than for an empty machine.
//
// The bytes must outlive the start, as with app_start_source(). ui/overlay.c
// is the only caller; it reserves this much internal DRAM up front and refuses
// to start when the reservation fails, because the Rust core does not report
// running out of memory, it panics and reboots.
#define OVERLAY_GUEST_HEAP (48*1024)
esp_err_t app_start_overlay(const char *source, size_t length);
// One turn of an overlay session. Guest JavaScript only: nothing is rendered
// or presented here, because the shell composites the overlay's display list
// into its own frame.
esp_err_t app_overlay_tick(void);
void app_force_redraw(void);
esp_err_t app_tick(uint32_t buttons);
void app_stop(void);
void app_request_stop(void);
void app_report(void);
// The last exception a Playground run reported, or "" when it ran clean.
const char *app_error(void);
