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
