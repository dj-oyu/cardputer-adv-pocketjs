#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#define LCD_W 240
#define LCD_H 135
#define STRIP_H 8
typedef enum { KEY_NONE, KEY_ENTER, KEY_BACK, KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN } board_key_t;
typedef struct { uint8_t row, col; bool pressed; } board_keyevent_t;
esp_err_t board_init(void);
// Brings up the SPI3 bus the microSD slot and the EXT connector share, once.
// Idempotent, lazy, and never released: callers add a device, they do not own
// the bus. See the comment at the definition for why it lives here.
esp_err_t board_spi3_acquire(void);
bool board_key_event(board_keyevent_t *out);
// The one strip buffer every screen draws into; board_present consumes it.
uint16_t *board_strip(void);
// TEMPORARY A/B switch for the panel transfer (see board.c): 1 = queue the strip
// and reap the previous one on the next call (asynchronous, the shipping path),
// 0 = the blocking polling transfer this file has always used. Both paths live in
// one binary so a later A/B measures the path and not the code placement.
extern int g_board_async;
// TEMPORARY A/B switch (see board.c): 1 = the queued path swaps straight into the
// panel buffer, 0 = swap in place and memcpy.
extern int g_board_swap_into;
// The same switch through a function, for the PERF report: shell.c flips it once
// per 2-second window so the queued and the blocking path are measured inside one
// binary (see shell.c). Reported as `async=` on the PERF line.
int board_async_get(void);
void board_async_set(int on);
esp_err_t board_present(int y, int rows, uint16_t *pixels);
// Columns [x, x+cols) of `rows` strip rows, each row read at pixels+row*240+x.
// The rest of the strip is not read and not sent, which is the point: a caller
// that recomposited six columns of a band pays for six columns on the wire. The
// panel is re-addressed for the window, so a partial-width transfer always
// costs the three commands the full-width path avoids -- worth it below roughly
// 200 columns, not worth it at 240, and board_present stays the way to say 240.
esp_err_t board_present_rect(int x, int y, int cols, int rows, uint16_t *pixels);
// Owner-task barrier: acknowledges completed transfer, including prior work.
// A transfer failure invalidates the panel write position.
esp_err_t board_present_sync(int y, int rows, uint16_t *pixels);
esp_err_t board_present_rect_sync(int x, int y, int cols, int rows, uint16_t *pixels);
uint16_t board_rgb(unsigned r, unsigned g, unsigned b);
void board_capture(bool enabled);
// Whether board_capture is dumping rows. A partial-width transfer would leave
// the untouched columns of the shared strip holding the previous band, and the
// PIX lines print whole rows, so the caller that narrows has to ask first.
bool board_capture_active(void);

// Battery voltage at the pack, and when it was sampled. There is deliberately
// no percentage and no charging flag here: the board has no fuel gauge and no
// charger status line, and a state of charge guessed from one voltage of an
// uncharacterised cell would be a number that looks measured and is not.
typedef struct { int millivolts; int64_t time_us; } board_battery_t;
// False if the ADC or its calibration is unavailable, in which case there is no
// reading to report rather than a bad one.
bool board_battery_read(board_battery_t *out);
