// See lcd_spi.h. The register sequence is spi_master's own for a send-only,
// full-duplex DMA transaction (ESP-IDF v6.0.1 spi_master.c spi_new_trans and
// s_spi_dma_prepare_data, spi_hal_iram.c spi_hal_setup_trans), minus what
// does not change between pixel strips. Line mode, dummy, command/address
// lengths, CS selection and clock stay as spi_master last wrote them: every
// transaction on this device, command or pixels, has the same shape and only
// its length and buffer differ.
#include "lcd_spi.h"
#include "esp_private/spi_common_internal.h"
#include "esp_private/gdma.h"
#include "hal/spi_ll.h"
#include "esp_timer.h"

static spi_dev_t *hw;
static gdma_channel_handle_t chan;
// One descriptor holds up to 4092 bytes and a strip is at most 3840; the
// second is the headroom spicommon_dma_desc_setup_link would need if a strip
// ever grew. Internal RAM (.bss), as the TRM requires (p.363).
static spi_dma_desc_t desc[2] __attribute__((aligned(DMA_DESC_MEM_ALIGN_SIZE)));
static bool busy;
static uint32_t underruns;

esp_err_t lcd_spi_direct_init(void) {
    spi_dma_ctx_t *ctx = spi_bus_get_dma_ctx(SPI2_HOST);
    if (!ctx || !ctx->tx_dma_chan) return ESP_ERR_INVALID_STATE;
    chan = ctx->tx_dma_chan;
    hw = SPI_LL_GET_HW(SPI2_HOST);
    return ESP_OK;
}

// spi_hal.c does the same around segmented transfers (sct_init/deinit).
void lcd_spi_direct_enter(void) { spi_ll_disable_int(hw); }
void lcd_spi_direct_leave(void) { spi_ll_enable_int(hw); }

void lcd_spi_direct_start(const void *data, size_t bytes) {
    spicommon_dma_desc_setup_link(desc, data, (int)bytes, false);
    spi_ll_clear_int_stat(hw);
    spi_ll_set_mosi_bitlen(hw, bytes * 8);
    spi_ll_set_miso_bitlen(hw, bytes * 8);
    // TRM p.1126 step 6 / p.367 step 1: reset both FIFOs' sides before every
    // transfer, so nothing left from the last one goes out first (p.1118).
    gdma_reset(chan);
    spi_ll_dma_tx_fifo_reset(hw);
    spi_ll_outfifo_empty_clr(hw);
    spi_ll_dma_tx_enable(hw, 1);
    gdma_start(chan, (intptr_t)desc);
    spi_ll_enable_mosi(hw, 1);
    spi_ll_enable_miso(hw, 0);
    spi_ll_apply_config(hw);
    spi_ll_user_start(hw);
    busy = true;
}

esp_err_t lcd_spi_direct_wait(void) {
    if (!busy) return ESP_OK;
    const int64_t began = esp_timer_get_time();
    while (!spi_ll_usr_is_done(hw)) {
        if (esp_timer_get_time() - began > 20000) { busy = false; return ESP_ERR_TIMEOUT; }
    }
    busy = false;
    if (hw->dma_int_raw.outfifo_empty_err) underruns++;
    return ESP_OK;
}

bool lcd_spi_direct_busy(void) { return busy; }
uint32_t lcd_spi_direct_underruns(void) { return underruns; }

// ---- The row ring (lcd_spi.h) -----------------------------------------------
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include <string.h>

typedef struct { spi_dma_desc_t *head; uint32_t bits; bool valid; } ring_band;
static uint16_t *ring_rows[LCD_SPI_RING_ROWS];
static spi_dma_desc_t ring_desc[LCD_SPI_RING_ROWS] __attribute__((aligned(DMA_DESC_MEM_ALIGN_SIZE)));
static unsigned ring_head;
// Shared with the GDMA interrupt, which may run on the other core only if
// init was called from there; it is called from the UI task (audit R4).
static portMUX_TYPE ring_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile ring_band ring_active, ring_pending;
#define RING_TIMEOUT_US 20000
// Every ring row free again (owner CPU): after a dropped transfer, whose rows
// the GDMA never wrote back, and on leaving the ring.
static void ring_release_rows(void) {
    for (int k = 0; k < LCD_SPI_RING_ROWS; k++) ring_desc[k].dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_CPU;
}

static void IRAM_ATTR ring_start(spi_dma_desc_t *head, uint32_t bits) {
    spi_ll_clear_int_stat(hw);
    spi_ll_set_mosi_bitlen(hw, bits);
    spi_ll_set_miso_bitlen(hw, bits);
    gdma_reset(chan);
    spi_ll_dma_tx_fifo_reset(hw);
    spi_ll_outfifo_empty_clr(hw);
    spi_ll_dma_tx_enable(hw, 1);
    gdma_start(chan, (intptr_t)head);
    spi_ll_enable_mosi(hw, 1);
    spi_ll_enable_miso(hw, 0);
    spi_ll_apply_config(hw);
    spi_ll_user_start(hw);
}

// The GDMA's end of frame: the active band's last byte has left the GDMA. The
// SPI may still be shifting its last few bytes, so wait for SPI_TRANS_DONE
// (CS up) before starting the next (audit R5). spi_master's own command
// transfers raise this too; they only run with the ring idle, so they find
// nothing active and are ignored.
static bool IRAM_ATTR ring_on_eof(gdma_channel_handle_t c, gdma_event_data_t *d, void *u) {
    (void)c; (void)d; (void)u;
    portENTER_CRITICAL_ISR(&ring_mux);
    if (ring_active.valid) {
        while (!spi_ll_usr_is_done(hw)) {}
        if (hw->dma_int_raw.outfifo_empty_err) underruns++;
        ring_active.valid = false;
        if (ring_pending.valid) {
            ring_active = ring_pending;
            ring_pending.valid = false;
            ring_start(ring_active.head, ring_active.bits);
        }
    }
    portEXIT_CRITICAL_ISR(&ring_mux);
    return false;
}

esp_err_t lcd_spi_ring_init(uint16_t *const rows[LCD_SPI_RING_ROWS]) {
    for (int k = 0; k < LCD_SPI_RING_ROWS; k++) ring_rows[k] = rows[k];
    gdma_tx_event_callbacks_t cbs = {.on_trans_eof = ring_on_eof};
    return gdma_register_tx_event_callbacks(chan, &cbs, NULL);
}

void lcd_spi_ring_enter(void) {
    lcd_spi_direct_enter();
    // Write each descriptor's owner bit back to CPU once its row is used:
    // that is how the copy knows a row is free (TRM p.360, p.379).
    gdma_strategy_config_t s = {.owner_check = false, .auto_update_desc = true,
                                .eof_till_data_popped = true};
    gdma_apply_strategy(chan, &s);
}

void lcd_spi_ring_leave(void) {
    gdma_strategy_config_t s = {.owner_check = false, .auto_update_desc = false,
                                .eof_till_data_popped = true};
    gdma_apply_strategy(chan, &s);
    ring_release_rows();
    lcd_spi_direct_leave();
}

esp_err_t lcd_spi_ring_submit(const uint16_t *pixels, int x, int cols, int rows) {
    const size_t row_bytes = (size_t)cols * 2u;
    spi_dma_desc_t *head = &ring_desc[ring_head], *prev = NULL;
    const int64_t began = esp_timer_get_time();
    for (int r = 0; r < rows; r++) {
        const unsigned k = ring_head;
        spi_dma_desc_t *dsc = &ring_desc[k];
        // Free once the GDMA has written the owner back (or never used).
        while (((volatile spi_dma_desc_t *)dsc)->dw0.owner == DMA_DESCRIPTOR_BUFFER_OWNER_DMA) {
            if (esp_timer_get_time() - began > RING_TIMEOUT_US) return ESP_ERR_TIMEOUT;
        }
        memcpy(ring_rows[k], pixels + r * 240 + x, row_bytes);
        dsc->dw0.size = row_bytes;
        dsc->dw0.length = row_bytes;
        dsc->dw0.suc_eof = r == rows - 1;
        dsc->dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_CPU;
        dsc->buffer = ring_rows[k];
        dsc->next = NULL;
        if (prev) prev->next = dsc;
        prev = dsc;
        ring_head = (ring_head + 1) % LCD_SPI_RING_ROWS;
    }
    // Hand the band to the GDMA only when every row is in place.
    for (spi_dma_desc_t *dsc = head; dsc; dsc = dsc->next) dsc->dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
    __sync_synchronize();
    const uint32_t bits = (uint32_t)(row_bytes * (size_t)rows * 8u);
    for (;;) {
        portENTER_CRITICAL(&ring_mux);
        if (!ring_active.valid) {
            ring_active = (ring_band){head, bits, true};
            ring_start(head, bits);
            portEXIT_CRITICAL(&ring_mux);
            return ESP_OK;
        }
        if (!ring_pending.valid) {
            ring_pending = (ring_band){head, bits, true};
            portEXIT_CRITICAL(&ring_mux);
            return ESP_OK;
        }
        portEXIT_CRITICAL(&ring_mux);
        if (esp_timer_get_time() - began > RING_TIMEOUT_US) return ESP_ERR_TIMEOUT;
    }
}

esp_err_t lcd_spi_ring_drain(void) {
    const int64_t began = esp_timer_get_time();
    while (ring_active.valid || ring_pending.valid) {
        if (esp_timer_get_time() - began > RING_TIMEOUT_US) {
            // Hung: drop both so the next command can reset the bus.
            portENTER_CRITICAL(&ring_mux);
            ring_active.valid = false;
            ring_pending.valid = false;
            portEXIT_CRITICAL(&ring_mux);
            ring_release_rows();
            return ESP_ERR_TIMEOUT;
        }
    }
    return ESP_OK;
}
