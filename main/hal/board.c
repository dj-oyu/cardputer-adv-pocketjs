#include "board.h"
#include "motion.h"
#include "sound.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static spi_device_handle_t lcd;
static i2c_master_dev_handle_t keyboard;
// One strip for the whole firmware. The home screen, both editors and a running
// app all draw from ui_task's single loop and board_present transmits
// synchronously, so no two of them ever hold pixels at the same time. Five
// private copies of this used to cost 15 KB of the 512 KB budget.
// If the transfer ever becomes an async DMA queue, this has to split in two.
// 16, not 4: the PIE 128-bit accesses below force the low four address bits to
// zero rather than faulting, so a misaligned buffer would silently read and
// write somewhere else. A row is 480 bytes, itself a multiple of 16, so every
// row start lands correctly once the base does.
static uint16_t shared[LCD_W * STRIP_H] __attribute__((aligned(16)));
uint16_t *board_strip(void) { return shared; }
static bool capture;
void board_capture(bool enabled) {
    capture=enabled;
    printf(enabled?"CAPTURE_BEGIN 240 135\n":"CAPTURE_END\n");
}

static esp_err_t tx(bool data, const void *bytes, size_t n) {
    gpio_set_level(34, data);
    spi_transaction_t t = {.length = n * 8, .tx_buffer = bytes};
    return spi_device_polling_transmit(lcd, &t);
}
static esp_err_t command(uint8_t c, const void *data, size_t n) {
    esp_err_t e = tx(false, &c, 1);
    return e == ESP_OK && n ? tx(true, data, n) : e;
}
static esp_err_t kwrite(uint8_t reg, uint8_t value) {
    uint8_t data[] = {reg, value};
    return i2c_master_transmit(keyboard, data, 2, 30);
}
static esp_err_t kread(uint8_t reg, uint8_t *value) {
    return i2c_master_transmit_receive(keyboard, &reg, 1, value, 1, 30);
}

// ---------------------------------------------------------------------------
// The panel wants each RGB565 pixel's two bytes the other way round.
//
// The S3's PIE unit does sixteen pixels a pass: two 128-bit loads, a
// de-interleave that gathers the even bytes into one register and the odd into
// the other, then a re-interleave with the two registers exchanged (TRM
// 1.8.209 and 1.8.212). There are no intrinsics for any of it, so this is
// assembly, and it is kept out of line because loopgtz drives the hardware loop
// registers and must not sit inside a loop the compiler is also driving.
//
// PIE is coprocessor 3, saved and restored on a task switch like the FPU, so
// this is safe in a task and not in an interrupt.
//
// Nothing here is trusted on faith: board_init runs both versions over the same
// bytes and only enables this one if they agree exactly.
static bool pie_swap;

static void __attribute__((noinline)) swap_pie(uint16_t *pixels, unsigned blocks) {
    uint16_t *out=pixels;
    __asm__ volatile(
        "loopgtz %2, 1f\n"
        "  ee.vld.128.ip q0, %0, 16\n"
        "  ee.vld.128.ip q1, %0, 16\n"
        "  ee.vunzip.8   q0, q1\n"
        "  ee.vzip.8     q1, q0\n"
        "  ee.vst.128.ip q1, %1, 16\n"
        "  ee.vst.128.ip q0, %1, 16\n"
        "1:\n"
        : "+a"(pixels), "+a"(out)
        : "a"(blocks)
        : "memory");
}

static void swap_scalar(uint16_t *pixels, int count) {
    for(int i=0;i<count;i++) pixels[i]=(uint16_t)((pixels[i]<<8)|(pixels[i]>>8));
}

// 32 pixels is 64 bytes, two of the 32-byte blocks the vector loop consumes.
static bool swap_agrees(void) {
    static uint16_t reference[32] __attribute__((aligned(16)));
    static uint16_t vectored[32]  __attribute__((aligned(16)));
    for(int i=0;i<32;i++) reference[i]=vectored[i]=(uint16_t)(i*2477u+0x1234u);
    swap_scalar(reference,32);
    swap_pie(vectored,sizeof(vectored)/32);
    return memcmp(reference,vectored,sizeof(reference))==0;
}
uint16_t board_rgb(unsigned r, unsigned g, unsigned b) {
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

// ---------------------------------------------------------------------------
// Battery voltage.
//
// The pack reaches GPIO10 -- ADC1 channel 9 on the S3 -- through a 100k/100k
// divider, so the reading is half the cell voltage. Three sources agree:
// M5Unified's Power_Class.cpp gives board_M5CardputerADV _batAdcPin 10,
// _batAdcUnit 1 and _adc_ratio 2.0f; the official ADV pinout lists Battery_ADC
// on G10; and schematic v1.0 shows R8 and R12 as the two 100k legs.
//
// What the board does not have is anything to ask about charging. The charger
// is a TP4057 whose CHRG and STDBY pins drive only the indicator LED, there is
// no PMIC and no fuel gauge on the I2C bus, and M5Stack's own documentation
// says so in as many words. So this reports millivolts and nothing else: no
// percentage, since nobody here has characterised the cell, and no charging
// flag, since there is no wire to read one from. With USB plugged in the
// reading sits near a full pack whatever its real state, and with the side
// switch off the pack is disconnected entirely.
//
// None of the above has been checked against a meter on this unit yet.
static adc_oneshot_unit_handle_t battery_adc;
static adc_cali_handle_t battery_cali;
static SemaphoreHandle_t battery_lock;
static int64_t battery_time;
static int battery_mv;

static void battery_init(void) {
    adc_oneshot_unit_init_cfg_t unit = {.unit_id = ADC_UNIT_1};
    if (adc_oneshot_new_unit(&unit, &battery_adc) != ESP_OK) { battery_adc=NULL; goto absent; }
    adc_oneshot_chan_cfg_t chan = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12};
    if (adc_oneshot_config_channel(battery_adc, ADC_CHANNEL_9, &chan) != ESP_OK) goto absent;
    // Without the calibration curve the converter gives counts, not volts, and
    // the linear guess that would turn one into the other is exactly the kind
    // of number this file refuses to invent. No curve, no reading.
    adc_cali_curve_fitting_config_t cali = {.unit_id = ADC_UNIT_1, .chan = ADC_CHANNEL_9,
        .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12};
    if (adc_cali_create_scheme_curve_fitting(&cali, &battery_cali) != ESP_OK) battery_cali=NULL;
    battery_lock = xSemaphoreCreateMutex();
    if (!battery_lock) goto absent;
    ESP_LOGI("board", "Battery ADC on GPIO10 (ADC1 ch9, x2 divider), calibration: %s",
             battery_cali ? "curve fitting" : "absent, readings disabled");
    return;
absent:
    ESP_LOGW("board", "Battery ADC unavailable");
    battery_cali = NULL;
}

bool board_battery_read(board_battery_t *out) {
    if (!battery_adc || !battery_cali || !battery_lock) return false;
    xSemaphoreTake(battery_lock, portMAX_DELAY);
    int64_t now = esp_timer_get_time();
    // A pack cannot move quickly and power status is a synchronous call an app
    // may make every frame, so one burst of conversions every half second is
    // more than the value can justify. Eight of them because a single 12-bit
    // read on this part is visibly noisy.
    if (!battery_time || now-battery_time > 500000) {
        int total=0, taken=0;
        for (int i=0;i<8;i++) {
            int raw=0, mv=0;
            if (adc_oneshot_read(battery_adc, ADC_CHANNEL_9, &raw) != ESP_OK) continue;
            if (adc_cali_raw_to_voltage(battery_cali, raw, &mv) != ESP_OK) continue;
            total += mv; taken++;
        }
        if (taken) { battery_mv = total*2/taken; battery_time = now; }
    }
    bool have = battery_time != 0;
    if (have) { out->millivolts = battery_mv; out->time_us = battery_time; }
    xSemaphoreGive(battery_lock);
    return have;
}
esp_err_t board_init(void) {
    gpio_config_t g = {.pin_bit_mask = (1ULL<<33)|(1ULL<<34)|(1ULL<<38), .mode = GPIO_MODE_OUTPUT};
    ESP_ERROR_CHECK(gpio_config(&g));
    gpio_set_level(38, 0); gpio_set_level(33, 0); vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(33, 1); vTaskDelay(pdMS_TO_TICKS(120));
    spi_bus_config_t bus = {.mosi_io_num=35, .miso_io_num=-1, .sclk_io_num=36,
        .quadwp_io_num=-1, .quadhd_io_num=-1, .max_transfer_sz=sizeof(shared)};
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
    // 80MHz, not the 40MHz M5Stack ships. The panel's flex is short and the
    // ST7789 tolerates it: send went from 15.5ms to 9.1ms measured, and the
    // owner confirmed on the physical panel that nothing is corrupted. That
    // confirmation had to be by eye -- board_capture dumps the buffer before
    // the byte swap and the transfer, and MISO is unwired, so no software
    // check here can see what actually reaches the glass. Revert to 40000000
    // if any tearing or colour damage ever shows up.
    spi_device_interface_config_t dev = {.clock_speed_hz=80000000, .mode=0, .spics_io_num=37, .queue_size=1};
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &lcd));
    pie_swap=swap_agrees();
    // Says which clock is in the binary. Several sessions share this tree, and
    // a measurement once moved 6ms because a clock change nobody remembered
    // making was sitting in the working copy.
    ESP_LOGI("board","LCD SPI %d Hz, byte swap: %s",
             dev.clock_speed_hz, pie_swap?"PIE":"scalar (PIE disagreed)");
    ESP_ERROR_CHECK(command(0x01, NULL, 0)); vTaskDelay(pdMS_TO_TICKS(150));
    ESP_ERROR_CHECK(command(0x11, NULL, 0)); vTaskDelay(pdMS_TO_TICKS(120));
    uint8_t format=0x55, orientation=0x60;
    ESP_ERROR_CHECK(command(0x3a, &format, 1));
    ESP_ERROR_CHECK(command(0x36, &orientation, 1));
    ESP_ERROR_CHECK(command(0x21, NULL, 0));
    ESP_ERROR_CHECK(command(0x29, NULL, 0));
    gpio_set_level(38, 1);
    i2c_master_bus_config_t ib = {.i2c_port=I2C_NUM_0, .sda_io_num=8, .scl_io_num=9,
        .clk_source=I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt=7, .flags.enable_internal_pullup=true};
    i2c_master_bus_handle_t ih;
    ESP_ERROR_CHECK(i2c_new_master_bus(&ib, &ih));
    i2c_device_config_t kd = {.dev_addr_length=I2C_ADDR_BIT_LEN_7, .device_address=0x34, .scl_speed_hz=400000};
    ESP_ERROR_CHECK(i2c_master_bus_add_device(ih, &kd, &keyboard));
    if (i2c_master_probe(ih, 0x34, 50) != ESP_OK) return ESP_ERR_NOT_FOUND;
    // TCA8418: 7 electrical rows x 8 columns, internal debouncing enabled.
    ESP_ERROR_CHECK(kwrite(0x1d, 0x7f)); ESP_ERROR_CHECK(kwrite(0x1e, 0xff));
    ESP_ERROR_CHECK(kwrite(0x1f, 0));
    for (uint8_t r=0x29;r<=0x2b;r++) ESP_ERROR_CHECK(kwrite(r, 0));
    ESP_ERROR_CHECK(kwrite(0x01, 0x01));
    for (int i=0;i<10;i++) { uint8_t value; kread(0x04, &value); }
    ESP_ERROR_CHECK(kwrite(0x02, 0x1f));
    ESP_LOGI("board", "ADV keyboard detected; LCD 240x135 RGB565; no PSRAM");
    battery_init();
    motion_init(ih);
    sound_init(ih);
    return ESP_OK;
}
// The TCA8418 FIFO reports both edges: bit 7 set is a press, clear a release.
// Text input needs the releases to track which modifiers are still held, so
// this returns every event and leaves the interpretation to keymap.c.
bool board_key_event(board_keyevent_t *out) {
    uint8_t count=0, event=0;
    if (kread(0x03, &count) != ESP_OK || !(count & 15)) return false;
    if (kread(0x04, &event) != ESP_OK) return false;
    kwrite(0x02, 0x1f);
    int code=(event & 0x7f)-1;
    if (code < 0 || code/10 >= 7 || code%10 >= 8) return false;
    out->row=(code%10)%4;
    out->col=(code/10)*2+(code%10 >= 4);
    out->pressed=(event & 0x80)!=0;
    return true;
}
// Byte-swaps `pixels` in place and sends it. The caller repaints the strip
// before every present, so consuming the buffer costs nothing and saves a
// second one.
#include "pet_hub.h"
esp_err_t board_present(int y, int rows, uint16_t *pixels) {
    if (y<0 || rows<1 || rows>STRIP_H || y+rows>LCD_H) return ESP_ERR_INVALID_ARG;
    pet_hub_overlay(pixels,y,rows);
    if(capture) {
        static char line[LCD_W*4+1];
        const char *hex="0123456789abcdef";
        for(int r=0;r<rows;r++) {
            for(int x=0;x<LCD_W;x++) {
                uint16_t p=pixels[r*LCD_W+x];
                for(int n=0;n<4;n++)line[x*4+n]=hex[(p>>(12-4*n))&15];
            }
            line[LCD_W*4]=0;printf("PIX %d %s\n",y+r,line);
            fflush(stdout);
            // Diagnostic capture only: let USB drain before the next full row.
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    // ST7789's 240x135 visible window in landscape (MADCTL=0x60). Every caller
    // walks the whole panel top to bottom without skipping a row (see the
    // callers' `for(strip_y=0; strip_y<LCD_H; strip_y+=STRIP_H)` loops), so
    // the window is the same 135-row rectangle every frame. Setting it once
    // per frame and then only streaming RAMWR data cuts 17 strips' worth of
    // CASET/RASET (32 transactions) down to one: RAMWR is documented to stay
    // open -- the write pointer keeps auto-incrementing -- across CS toggles
    // until another command is sent, and nothing else here sends the panel a
    // command mid-frame. Nothing in software could confirm that -- board_capture
    // samples `pixels` before this point, and MISO is unwired -- so it was
    // checked on the physical panel, which is the only evidence there is. If
    // the picture ever tears or the ribbons land on the wrong rows, revert to
    // setting xs/ys and issuing 0x2c on every call, the way this used to work.
    if(y==0) {
        uint16_t x0=40, x1=279, y0=53, y1=53+LCD_H-1;
        uint8_t xs[]={x0>>8,x0,x1>>8,x1}, ys[]={y0>>8,y0,y1>>8,y1};
        esp_err_t e=command(0x2a,xs,4); if(e) return e;
        e=command(0x2b,ys,4); if(e) return e;
        e=command(0x2c,NULL,0); if(e) return e;   // RAMWR: opens the write session
    }
    // After the capture block above, which wants the pixels as drawn.
    //
    // A 32-bit C version of this was measured and was worse: this file builds
    // at -Os, where the four-byte memcpy that expresses an aligned wide access
    // stayed a call and the transfer went from 16.5 ms to 24.2 ms.
    int count=LCD_W*rows;
    if(pie_swap) swap_pie(pixels,(unsigned)(count*2/32));
    else swap_scalar(pixels,count);
    return tx(true,pixels,(size_t)LCD_W*rows*2);
}
