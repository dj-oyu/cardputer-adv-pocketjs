#pragma once
// The LCD's pixel strips sent straight to GP-SPI2 and its GDMA channel,
// without spi_master's per-transaction queue, bus lock, ISR and task wake
// (docs/platform/lcd-dma-driver.md). spi_master still owns the bus: it set
// up the pins, clock and DMA channel, and it still sends every command. This
// file only starts a pixel transfer and waits for it.
//
// UI task only. A transfer must be finished (lcd_spi_direct_wait) before
// anything else uses the bus, which board.c's tx_reap already guarantees for
// every command.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t lcd_spi_direct_init(void);
// Enter/leave direct use. Entering masks the SPI's transfer-done interrupt so
// spi_master's ISR cannot run on a transfer it did not start; leaving unmasks
// it again with the done bit still set, which is the idle state spi_master
// expects (it starts a queued transaction from the ISR that bit raises).
void lcd_spi_direct_enter(void);
void lcd_spi_direct_leave(void);
// One pixel transfer of `bytes` (1..4092) from internal RAM. DC must already
// be high. Returns at once; the data is on the wire until _wait returns.
void lcd_spi_direct_start(const void *data, size_t bytes);
// Spins until the SPI reports the transfer done (SPI_TRANS_DONE: CS has
// risen, so the last bit is on the wire). ESP_ERR_TIMEOUT after 20 ms.
esp_err_t lcd_spi_direct_wait(void);
bool lcd_spi_direct_busy(void);
// Transfers that ended with the out-FIFO-empty flag up (TRM p.1118: GDMA ran
// short of the SPI's length). Must stay 0; a diagnostic, not an error path.
uint32_t lcd_spi_direct_underruns(void);

// ---- The row ring (docs/platform/lcd-dma-driver.md 9.4, design 2) ----------
// Pixel strips are copied row by row into a ring of LCD_SPI_RING_ROWS rows,
// each with its own descriptor, as soon as the GDMA has consumed the row
// that was there (descriptor write-back). A band whose rows are all copied
// waits as the pending transfer; the GDMA end-of-frame interrupt starts it
// the moment the active one is done on the wire (SPI_TRANS_DONE). So the
// caller can copy band j+1 and go on drawing band j+2 while band j is still
// being sent, with 12 rows of transmit buffer rather than 16.
#define LCD_SPI_RING_ROWS 12
// rows[k]: the buffer of ring row k, 240 pixels, internal RAM. UI task only;
// the interrupt is installed on the calling core (the UI task's).
esp_err_t lcd_spi_ring_init(uint16_t *const rows[LCD_SPI_RING_ROWS]);
void lcd_spi_ring_enter(void);
void lcd_spi_ring_leave(void);
// Copies `rows` rows of `cols` pixels from pixels + r*240 + x into the ring
// and queues them as one transfer. Waits only for ring rows still in use.
esp_err_t lcd_spi_ring_submit(const uint16_t *pixels, int x, int cols, int rows);
// Waits until nothing is active or pending.
esp_err_t lcd_spi_ring_drain(void);
