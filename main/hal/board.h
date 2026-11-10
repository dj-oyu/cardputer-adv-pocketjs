#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#define LCD_W 240
#define LCD_H 135
#define STRIP_H 8
typedef enum { KEY_NONE, KEY_ENTER, KEY_BACK, KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN } board_key_t;
// `overflow`: the controller's FIFO filled and dropped events after this one
// (see board_key_event). The event itself is still valid.
typedef struct { uint8_t row, col; bool pressed, overflow; } board_keyevent_t;
esp_err_t board_init(void);
// Brings up the SPI3 bus the microSD slot and the EXT connector share, once.
// Idempotent, lazy, and never released: callers add a device, they do not own
// the bus. See the comment at the definition for why it lives here.
esp_err_t board_spi3_acquire(void);
bool board_key_event(board_keyevent_t *out);
// The one strip buffer every screen draws into; board_present consumes it.
uint16_t *board_strip(void);
// Zero-copy strips (docs/platform/lcd-dma-driver.md 9.4): with
// g_board_zero_copy set (BOARD_BUF_AB builds only, for now), the board's two strip buffers take turns as the draw
// target and a strip is sent from where it was drawn, with no copy. This
// returns the one that is not in flight. Needs LSB-first pixels (no swap).
// Off (0) it returns board_strip(). Frame boundaries only may change it.
uint16_t *board_strip_next(void);
// Design 0 (docs/platform/lcd-dma-driver.md 9.5): no tx_buf. Kasane and the
// home loop ask for 4-row strips with board_strip_next and send them from
// where they were drawn; any other screen's whole strip goes out and is
// finished before board_present returns. 0: today's copy into tx_buf.
#ifndef BOARD_STRIP_HALF
#define BOARD_STRIP_HALF 1
#endif
// The rows of the strips board_strip_next hands out (4 with BOARD_STRIP_HALF).
int board_strip_rows(void);
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
// panel window is reused for contiguous bands with identical x/cols. A change
// of bounds or a discontinuity costs three commands. The existing renderer
// narrow threshold and 16-pixel alignment are unchanged.
esp_err_t board_present_rect(int x, int y, int cols, int rows, uint16_t *pixels);
// Owner-task barrier: acknowledges completed transfer, including prior work.
// A transfer failure invalidates the panel write position.
esp_err_t board_present_sync(int y, int rows, uint16_t *pixels);
esp_err_t board_present_rect_sync(int x, int y, int cols, int rows, uint16_t *pixels);
#ifdef KASANE_DMA_OVERLAP
// UI owner only. Copies pixels before return; TX/descriptor remain board-owned
// until finish. Never acknowledge a frame until finish succeeds. A non-timeout
// driver error without descriptor return quarantines TX until a later finish.
esp_err_t board_present_submit(int x,int y,int cols,int rows,uint16_t *pixels);
esp_err_t board_present_finish(void);
#endif
// Measurement build (docs/platform/lcd-dma-driver.md, S0b): two panel buffers
// and queue_size 2 on spi_master, so band j is copied and queued while band
// j-1 is still on the wire and the driver's ISR starts j as soon as j-1 ends.
// g_board_depth flips 1/2 at run time for a same-binary A/B; 1 is today's
// behaviour (every submit waits for the previous band).
#ifndef BOARD_LCD_DEPTH2
#define BOARD_LCD_DEPTH2 0
#endif
#if BOARD_LCD_DEPTH2
extern int g_board_depth;
#endif
// Measurement build: a same-binary A/B of the panel byte order (LSB first,
// no swap) against MSB first with the swap. Call at a frame boundary.
#ifndef BOARD_ENDIAN_AB
#define BOARD_ENDIAN_AB 0
#endif
#if BOARD_ENDIAN_AB
int board_endian_ab_flip(void);
#endif
// Measurement build: pixel strips go straight to GP-SPI2 + GDMA (lcd_spi.c)
// instead of through spi_master's queue. board_lcd_direct_set flips it at a
// frame boundary for a same-binary A/B; returns the new state.
#ifndef BOARD_LCD_DIRECT
#define BOARD_LCD_DIRECT 1
#endif
#ifndef BOARD_BUF_AB
#define BOARD_BUF_AB 0
#endif
#if BOARD_BUF_AB
// Measurement build: 0 copy (today), 1 zero-copy strips (1'), 2 the strip's
// 4-row halves (design 0), 3 the row ring (docs/platform/lcd-dma-driver.md
// 9.4-9.5). At a frame boundary only.
extern int g_board_zero_copy;
int board_buf_mode_set(int mode);
int board_buf_mode(void);
uint32_t board_buf_underruns(void);
#endif
#if BOARD_LCD_DIRECT
int board_lcd_direct_set(int on);
uint32_t board_lcd_direct_underruns(void);
#endif
uint16_t board_rgb(unsigned r, unsigned g, unsigned b);
// Owner-task frame boundary, after transfer completion (success or failure).
// Ends window reuse only; it does not drain DMA or acknowledge a transfer.
void board_present_frame_end(void);
void board_capture(bool enabled);
// Whether board_capture is dumping rows. A partial-width transfer would leave
// the untouched columns of the shared strip holding the previous band, and the
// PIX lines print whole rows, so the caller that narrows has to ask first.
bool board_capture_active(void);
// One app display turn per BEGIN/END scope, including press/release wrappers.
// UI-owner only, like board_capture(); the next scope resets the allowance.
bool board_capture_take_turn(void);
#ifdef CONFIG_POCKET_FRAME_BREAKDOWN
// Strips queued to the SPI driver since the last call (FRAME_BD).
uint32_t board_lcd_tx_take(void);
#endif
#ifdef KASANE_P0_BUS_PROBE
// Observed by the SPI2 post-completion callback, not inferred from config.
int board_lcd_isr_core(void);
#endif

// Battery voltage at the pack, and when it was sampled. There is deliberately
// no percentage and no charging flag here: the board has no fuel gauge and no
// charger status line, and a state of charge guessed from one voltage of an
// uncharacterised cell would be a number that looks measured and is not.
typedef struct { int millivolts; int64_t time_us; } board_battery_t;
// False if the ADC or its calibration is unavailable, in which case there is no
// reading to report rather than a bad one.
bool board_battery_read(board_battery_t *out);
