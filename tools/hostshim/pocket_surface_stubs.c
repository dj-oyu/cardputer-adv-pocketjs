// pocket.kasane's grid, video and pixel surfaces, absent: host harnesses that
// link main/pocket/pocket_kasane.c without those surfaces (tools/
// build_kasane_test.sh, tools/vmtest/prof/stress_prof.sh) get these instead.
// The real headers are included so a signature change breaks this build
// rather than the link. Nothing is installed, nothing is ever pending.
// Weak, because the game harnesses (test_derby_host.c, test_big_wave_host.c,
// test_lcd_catch_host.c, ...) define their own and go through the same
// build_kasane_test.sh: theirs win, harnesses without them get these.
#include "pocket_grid.h"
#include "pocket_pixel.h"
#include "pocket_video.h"

__attribute__((weak)) esp_err_t pocket_grid_install(JSContext *ctx, JSValueConst kasane_namespace) { (void)ctx; (void)kasane_namespace; return ESP_OK; }
__attribute__((weak)) void pocket_grid_reset(void) {}
__attribute__((weak)) bool pocket_grid_pending(void) { return false; }
__attribute__((weak)) void pocket_grid_present_result(ksn_result result) { (void)result; }
__attribute__((weak)) void pocket_grid_source_invalidated(uint32_t resource_id) { (void)resource_id; }

__attribute__((weak)) esp_err_t pocket_pixel_install(JSContext *ctx, JSValueConst kasane) { (void)ctx; (void)kasane; return ESP_OK; }
__attribute__((weak)) void pocket_pixel_reset(void) {}
__attribute__((weak)) bool pocket_pixel_pending(void) { return false; }
__attribute__((weak)) void pocket_pixel_present_result(ksn_result result) { (void)result; }

__attribute__((weak)) esp_err_t pocket_video_install(JSContext *ctx, JSValueConst kasane) { (void)ctx; (void)kasane; return ESP_OK; }
__attribute__((weak)) void pocket_video_reset(void) {}
__attribute__((weak)) bool pocket_video_pending(void) { return false; }
__attribute__((weak)) void pocket_video_present_result(ksn_result result) { (void)result; }
