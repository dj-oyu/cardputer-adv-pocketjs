#include "board.h"
#include "motion.h"
#include "sound.h"
#include "ui/kasane/ksn_p0_probe.h"
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
#ifdef KASANE_P0_BUS_PROBE
#include <stdatomic.h>
#include "esp_attr.h"
#endif

static spi_device_handle_t lcd;
static i2c_master_dev_handle_t keyboard;
// One strip for the whole firmware. The home screen, both editors and a running
// app all draw from ui_task's single loop, so no two of them ever hold pixels at
// the same time. Five private copies of this used to cost 15 KB of the 512 KB
// budget. The transfer below is a queue and it does NOT split this buffer in two:
// it copies each strip into the panel's own tx_buf before queueing, so the caller
// can go straight on reusing this one for the next strip.
// 16, not 4: the PIE 128-bit accesses below force the low four address bits to
// zero rather than faulting, so a misaligned buffer would silently read and
// write somewhere else. A row is 480 bytes, itself a multiple of 16, so every
// row start lands correctly once the base does.
static uint16_t shared[LCD_W * STRIP_H] __attribute__((aligned(16)));
uint16_t *board_strip(void) { return shared; }
// The A/B switch for the asynchronous transfer below: 1 = queue the strip and
// come back for its result on the next call (the shipping path), 0 = the
// blocking polling transfer this file has always used. Both paths are in ONE
// binary because the same kernel moves 15% between builds from instruction-cache
// alignment alone (CLAUDE.md), and the queued-vs-blocking question was smaller
// than that.
//
// It is FIXED at 1 now. shell.c used to invert it once per 2-second PERF window
// so adjacent windows of the same run were the two paths; that measurement is
// done (docs/perf/pie-consolidation.md 3b: FLOWER draw 38.73 -> 33.30 ms, fps
// 24.95 -> 28.80) and a shipped build must not spend every other window on the
// path it rejected. The setter stays for the next A/B -- nothing calls it in
// this build.
int g_board_async = 1;
// The same switch through a function, for the PERF report.
int board_async_get(void) { return g_board_async; }
void board_async_set(int on) { g_board_async = on ? 1 : 0; }
// TEMPORARY A/B switch inside the queued path: 1 = byte-swap straight into the
// panel buffer that is not in flight (one pass, `shared` left as drawn), 0 = swap
// `shared` in place and memcpy it across, as 7fd965f shipped. Same pixels on the
// wire either way; one binary for the same reason as g_board_async.
int g_board_swap_into = 1;
// The panel's own copies. `shared` stays the one buffer every screen draws into
// (17 call sites hold that pointer for a whole frame), so the asynchronous path
// COPIES a strip here instead of handing the caller a second buffer: 3,840 bytes
// of memcpy is ~10 us against the ~440 us of SPI the copy lets the CPU skip.
// Two buffers, because one is in flight while the next strip is copied.
static uint16_t tx_buf[2][LCD_W * STRIP_H] __attribute__((aligned(16)));
static int tx_front;
static bool tx_inflight;
// The descriptor must outlive the queued transaction: spi_device_queue_trans
// keeps the pointer until the result is reaped.
static spi_transaction_t tx_pending;
#ifdef KASANE_P0_BUS_PROBE
/* Diagnostic only. SPI post_cb runs in the SPI2 ISR just before the driver
 * places the result on ret_queue. This is an ISR-service timestamp, not the
 * exact hardware DMA completion edge. Both tasks use the same esp_timer clock. */
static atomic_uint lcd_data_isr_us;
static atomic_int lcd_data_isr_core=ATOMIC_VAR_INIT(-1);
static bool lcd_isr_reported;
int board_lcd_isr_core(void){
    return atomic_load_explicit(&lcd_data_isr_core,memory_order_relaxed);
}
static void IRAM_ATTR lcd_post_cb(spi_transaction_t *trans){
    if(trans->user==&lcd_data_isr_us){
        atomic_store_explicit(&lcd_data_isr_core,xPortGetCoreID(),memory_order_relaxed);
        atomic_store_explicit(&lcd_data_isr_us,(uint32_t)esp_timer_get_time(),
                              memory_order_relaxed);
    }
}
#endif
// Reap the strip queued last time. One transaction is in flight at a time
// (the LCD device is configured with queue_size=1), so this is also the barrier
// every command goes through before it ends the RAMWR session. Returns the
// transfer's own error, or ESP_OK when there was nothing in flight.
static esp_err_t tx_reap(void) {
    if (!tx_inflight) return ESP_OK;
#ifdef KASANE_P0_BUS_PROBE
    bool sd_before=ksn_p0_bus_sd_active();
    uint32_t sd_epoch_before=ksn_p0_bus_sd_epoch();
    int64_t began=esp_timer_get_time();
#endif
    spi_transaction_t *done = NULL;
    esp_err_t e = spi_device_get_trans_result(lcd, &done, portMAX_DELAY);
#ifdef KASANE_P0_BUS_PROBE
    uint32_t finished=(uint32_t)esp_timer_get_time();
    uint32_t elapsed=finished-(uint32_t)began;
    ksn_p0_bus_phase_sample(KSN_P0_BUS_REAP,
        elapsed,
        sd_before||ksn_p0_bus_sd_active()||
        sd_epoch_before!=ksn_p0_bus_sd_epoch());
    uint32_t isr=atomic_load_explicit(&lcd_data_isr_us,memory_order_relaxed);
    if(isr){
        /* The ISR may already have run before the UI entered get_result. */
        uint32_t pre=(int32_t)(isr-(uint32_t)began)>0?isr-(uint32_t)began:0u;
        if(pre>elapsed)pre=elapsed;
        ksn_p0_bus_phase_sample(KSN_P0_BUS_PRE_ISR,pre,false);
        ksn_p0_bus_phase_sample(KSN_P0_BUS_POST_ISR,elapsed-pre,false);
    }else ksn_p0_bus_missing_isr();
    if(!lcd_isr_reported){
        int core=atomic_load_explicit(&lcd_data_isr_core,memory_order_relaxed);
        if(core>=0){
            lcd_isr_reported=true;
            ESP_LOGI("board","P1 LCD ISR observed core %d, UI reap core %d",
                     core,xPortGetCoreID());
        }
    }
#endif
    tx_inflight = false;
    return e;
}
static bool capture;
void board_capture(bool enabled) {
    capture=enabled;
    printf(enabled?"CAPTURE_BEGIN 240 135\n":"CAPTURE_END\n");
}
bool board_capture_active(void) { return capture; }

static esp_err_t tx(bool data, const void *bytes, size_t n) {
    gpio_set_level(34, data);
    spi_transaction_t t = {.length = n * 8, .tx_buffer = bytes};
    return spi_device_polling_transmit(lcd, &t);
}
// The panel row the open RAMWR session will write next, or -1 when no session
// is open. board_present() streams into an open one rather than re-addressing
// the window on every strip, so it needs to know where the pointer is; any
// command at all ends the session, which is why this is cleared here instead
// of at the call sites.
static int next_row = -1;

static esp_err_t command(uint8_t c, const void *data, size_t n) {
    // Any command at all ends the RAMWR session, so nothing may be in flight
    // when one starts: reap first (the queued strip is still ordered before it).
    // next_row is -1 either way, because a command ends the session.
    (void)tx_reap();
    next_row = -1;
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
// The display path was suspected of racing commands against a strip transfer. It
// does not: tx_reap() waits with portMAX_DELAY, command() calls it before every
// command, and tx()'s first argument is the DC line rather than a byte swap. The
// instruments that were going to prove otherwise measured nothing (they were taken
// while the UI task was blocked, see the note in shell.c) and are not kept.
static bool pie_swap;

// `in` and `out` may be the same buffer (the blocking path swaps in place) or
// two different aligned buffers (the queued path swaps straight into the panel's
// own copy): each block is loaded whole before either half is stored.
static void __attribute__((noinline)) swap_pie(uint16_t *in, uint16_t *out, unsigned blocks) {
    __asm__ volatile(
        "loopgtz %2, 1f\n"
        "  ee.vld.128.ip q0, %0, 16\n"
        "  ee.vld.128.ip q1, %0, 16\n"
        "  ee.vunzip.8   q0, q1\n"
        "  ee.vzip.8     q1, q0\n"
        "  ee.vst.128.ip q1, %1, 16\n"
        "  ee.vst.128.ip q0, %1, 16\n"
        "1:\n"
        : "+a"(in), "+a"(out)
        : "a"(blocks)
        : "memory");
}

static void swap_scalar(uint16_t *out, const uint16_t *in, int count) {
    for(int i=0;i<count;i++) out[i]=(uint16_t)((in[i]<<8)|(in[i]>>8));
}

// 32 pixels is 64 bytes, two of the 32-byte blocks the vector loop consumes.
// Both spellings the transfer uses are checked: in place, and into another buffer.
static bool swap_agrees(void) {
    static uint16_t reference[32] __attribute__((aligned(16)));
    static uint16_t vectored[32]  __attribute__((aligned(16)));
    static uint16_t copied[32]    __attribute__((aligned(16)));
    for(int i=0;i<32;i++) reference[i]=vectored[i]=(uint16_t)(i*2477u+0x1234u);
    swap_pie(vectored,copied,sizeof(vectored)/32);
    swap_scalar(reference,reference,32);
    swap_pie(vectored,vectored,sizeof(vectored)/32);
    return memcmp(reference,vectored,sizeof(reference))==0 &&
           memcmp(reference,copied,sizeof(reference))==0;
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
// ------------------------------------------------------------------ SPI3 bus
//
// The microSD slot (CS=12) and the EXT connector (CS=5) share MOSI=14, CLK=40
// and MISO=39 (docs/platform/hardware-constraints.md:45). The LCD uses SPI2,
// so card traffic cannot occupy its SPI device queue. Both hosts may still
// contend for shared DMA/memory bandwidth; see the P1 LCD/SD diagnostic.
//
// The bus lives here, beside the LCD's, rather than inside whichever driver
// happens to come up first. Exactly one caller may spi_bus_initialize a host;
// two owners is a real failure and putting this next to the other bus owner is
// what stops it being possible. Drivers add their own DEVICES and interleave at
// transaction granularity, which the SPI master driver already serialises -- so
// a mounted card does not exclude io.spi when that arrives, and neither has to
// know about the other.
//
// Lazy: a board that never mounts a card should not pay for a DMA channel. And
// never torn down, because by then a second device may be sharing it.
static bool spi3_up;

esp_err_t board_spi3_acquire(void) {
    if (spi3_up) return ESP_OK;
    spi_bus_config_t bus = {.mosi_io_num=14, .miso_io_num=39, .sclk_io_num=40,
        .quadwp_io_num=-1, .quadhd_io_num=-1, .max_transfer_sz=4096};
    esp_err_t e = spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO);
    if (e != ESP_OK) return e;
    spi3_up = true;
    ESP_LOGI("board","SPI3 up: SD CS=12, EXT CS=5 share MOSI=14 CLK=40 MISO=39");
    return ESP_OK;
}

esp_err_t board_init(void) {
    gpio_config_t g = {.pin_bit_mask = (1ULL<<33)|(1ULL<<34)|(1ULL<<38), .mode = GPIO_MODE_OUTPUT};
    ESP_ERROR_CHECK(gpio_config(&g));
    gpio_set_level(38, 0); gpio_set_level(33, 0); vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(33, 1); vTaskDelay(pdMS_TO_TICKS(120));
    spi_bus_config_t bus = {.mosi_io_num=35, .miso_io_num=-1, .sclk_io_num=36,
        .quadwp_io_num=-1, .quadhd_io_num=-1, .max_transfer_sz=sizeof(shared)
#ifdef KASANE_P1_LCD_ISR_CORE1
        ,.isr_cpu_id=ESP_INTR_CPU_AFFINITY_1
#endif
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
#ifdef KASANE_P1_LCD_ISR_CORE1
    ESP_LOGI("board","P1 LCD ISR affinity requested core 1, UI core %d",
             CONFIG_POCKET_UI_TASK_CORE);
#endif
    // 80MHz, not the 40MHz M5Stack ships. The panel's flex is short and the
    // ST7789 tolerates it: send went from 15.5ms to 9.1ms measured, and the
    // owner confirmed on the physical panel that nothing is corrupted. That
    // confirmation had to be by eye -- board_capture dumps the buffer before
    // the byte swap and the transfer, and MISO is unwired, so no software
    // check here can see what actually reaches the glass. Revert to 40000000
    // if any tearing or colour damage ever shows up.
    spi_device_interface_config_t dev = {.clock_speed_hz=80000000, .mode=0, .spics_io_num=37, .queue_size=1
#ifdef KASANE_P0_BUS_PROBE
        ,.post_cb=lcd_post_cb
#endif
    };
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
#include "pocket_capture.h"
esp_err_t board_present(int y, int rows, uint16_t *pixels) {
    if (y<0 || rows<1 || rows>STRIP_H || y+rows>LCD_H) return ESP_ERR_INVALID_ARG;
    pet_hub_overlay(pixels,y,rows);
    // docs/api/common-api.md 9 makes showing that the microphone is live the
    // host's obligation, so it is drawn here, at the one transfer to the
    // panel, rather than by whichever screen happens to be up: an app can
    // paint the corner it occupies, but not after this.
    pocket_capture_overlay(pixels,y,rows);
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
    // ST7789's 240x135 visible window in landscape (MADCTL=0x60). Most callers
    // walk the whole panel top to bottom without skipping a row (their
    // `for(strip_y=0; strip_y<LCD_H; strip_y+=STRIP_H)` loops), and for those
    // the window is set once per frame and the rest is streamed: RAMWR stays
    // open -- the write pointer keeps auto-incrementing -- across CS toggles
    // until another command is sent, and nothing else here sends the panel a
    // command mid-frame. That cuts 17 strips' worth of CASET/RASET (32
    // transactions) down to one.
    //
    // The condition is where the pointer is, not `y==0`. It used to be the
    // latter, which was the same test while every caller sent every strip.
    // main/ui/codeedit.c now sends only the strips it changed, and under the
    // old test a frame that skipped strip 0 set no window at all: its rows
    // went wherever the pointer happened to be, and a frame that skipped a
    // middle strip pulled everything below it up by 8 rows. Neither is
    // visible from software -- board_capture samples `pixels` above, before
    // this point, and MISO is unwired -- so it would have shown only on the
    // glass. Tracking the pointer costs a re-window per discontinuity and
    // nothing at all for a caller that sends every strip.
    if(y!=next_row) {
        uint16_t x0=40, x1=279, y0=53+(uint16_t)y, y1=53+LCD_H-1;
        uint8_t xs[]={x0>>8,x0,x1>>8,x1}, ys[]={y0>>8,y0,y1>>8,y1};
        esp_err_t e=command(0x2a,xs,4); if(e) return e;
        e=command(0x2b,ys,4); if(e) return e;
        e=command(0x2c,NULL,0); if(e) return e;   // RAMWR: opens the write session
    }
    // The byte swap goes after the capture block above, which wants the pixels
    // as drawn.
    //
    // A 32-bit C version of this was measured and was worse: this file builds
    // at -Os, where the four-byte memcpy that expresses an aligned wide access
    // stayed a call and the transfer went from 16.5 ms to 24.2 ms.
    int count=LCD_W*rows;
    size_t bytes = (size_t)LCD_W * rows * 2;
    esp_err_t e;
    if (g_board_async) {
#ifdef KASANE_P0_BUS_PROBE
        int64_t swap_began=esp_timer_get_time();
#endif
        // THE PIPELINE. The strip queued on the previous call has been going out
        // during everything above (~440 us of SPI against ~2.2 ms of drawing and
        // layout). The panel buffer that is NOT in flight is tx_buf[tx_front], so
        // the swap writes straight into it while that transfer is still going:
        // the swap and the copy are one pass, and neither waits for the wire.
        // `shared` is left as drawn. Then the previous result is reaped (one
        // transfer in flight at a time, which is what queue_size=1 allows) and
        // this strip is queued.
        uint16_t *panel = tx_buf[tx_front];
        if (g_board_swap_into) {
            if(pie_swap) swap_pie(pixels,panel,(unsigned)(count*2/32));
            else swap_scalar(panel,pixels,count);
        } else {
            // The A/B arm: swap in place, then copy, the way 7fd965f shipped it.
            if(pie_swap) swap_pie(pixels,pixels,(unsigned)(count*2/32));
            else swap_scalar(pixels,pixels,count);
            memcpy(panel, pixels, bytes);
        }
#ifdef KASANE_P0_BUS_PROBE
        ksn_p0_bus_phase_sample(KSN_P0_BUS_SWAP,
            (uint32_t)(esp_timer_get_time()-swap_began),false);
#endif
        e = tx_reap();
        if (e == ESP_OK) {
            // DC high: these bytes are pixel data and not a command. tx() is the
            // only other place that touches the line and it cannot be used here,
            // because it would block on the transfer this path exists to overlap.
            gpio_set_level(34, 1);
            tx_pending = (spi_transaction_t){.length = bytes * 8, .tx_buffer = panel};
#ifdef KASANE_P0_BUS_PROBE
            atomic_store_explicit(&lcd_data_isr_us,0u,memory_order_relaxed);
            tx_pending.user=&lcd_data_isr_us;
            int64_t queue_began=esp_timer_get_time();
#endif
            e = spi_device_queue_trans(lcd, &tx_pending, portMAX_DELAY);
#ifdef KASANE_P0_BUS_PROBE
            ksn_p0_bus_phase_sample(KSN_P0_BUS_QUEUE,
                (uint32_t)(esp_timer_get_time()-queue_began),false);
#endif
            tx_inflight = (e == ESP_OK);
            tx_front ^= 1;
        }
    } else {
        if(pie_swap) swap_pie(pixels,pixels,(unsigned)(count*2/32));
        else swap_scalar(pixels,pixels,count);
        e = tx_reap();
        if (e == ESP_OK) e = tx(true, pixels, bytes);
    }
    // Where the pointer lands once these rows are in. At the bottom of the
    // window it wraps to the window's own top, which is only row 0 when this
    // frame started there, so the end of the panel always re-windows. A failed
    // transfer leaves the pointer unknown, which is what -1 means.
    next_row = (e==ESP_OK && y+rows<LCD_H) ? y+rows : -1;
    return e;
}

// The narrow sibling of board_present. It always re-addresses the panel: the
// window is not the full-width one the streaming path keeps open, and the write
// pointer wraps to the WINDOW's left edge at the end of each row, which is what
// makes a partial-width RAMWR work at all. next_row is cleared afterwards so
// the next full-width strip re-windows instead of streaming into this one.
//
// The overlays still paint their whole rectangles into the strip and only the
// window goes out. That is correct rather than lucky: everything they draw
// outside the window is identical to what is already on the glass, because the
// frame that put it there was full width (an overlay's own change comes through
// app_force_redraw(), which asks for whole bands).
esp_err_t board_present_rect(int x, int y, int cols, int rows, uint16_t *pixels) {
    if (x<0 || cols<1 || x+cols>LCD_W) return ESP_ERR_INVALID_ARG;
    if (y<0 || rows<1 || rows>STRIP_H || y+rows>LCD_H) return ESP_ERR_INVALID_ARG;
    if (cols==LCD_W) return board_present(y,rows,pixels);
    pet_hub_overlay(pixels,y,rows);
    pocket_capture_overlay(pixels,y,rows);
    // No capture arm: board_capture dumps whole rows and the columns outside
    // the window hold the previous band, so a caller that is capturing must not
    // narrow. board_capture_active() is how it finds that out.
    uint16_t px0=40+(uint16_t)x, px1=40+(uint16_t)(x+cols)-1;
    uint16_t py0=53+(uint16_t)y, py1=53+(uint16_t)(y+rows)-1;
    uint8_t xs[]={px0>>8,px0,px1>>8,px1}, ys[]={py0>>8,py0,py1>>8,py1};
    esp_err_t e=command(0x2a,xs,4); if(e) { next_row=-1; return e; }
    e=command(0x2b,ys,4); if(e) { next_row=-1; return e; }
    e=command(0x2c,NULL,0); if(e) { next_row=-1; return e; }
    size_t bytes=(size_t)cols*rows*2;
    // Rows are packed as they are swapped, so the window leaves the strip in one
    // pass exactly as the full-width path does. Both arms pack into a panel
    // buffer rather than in place: the strip's rows are 240 apart and the
    // window's are `cols` apart, so there is a copy either way, and packing into
    // the buffer that is not in flight keeps the queued arm's overlap.
    //
    // The kernel arm is the reason ksn_render rounds its window out to 16
    // pixels: 16 pixels is 32 bytes, so an aligned x and a multiple-of-16 cols
    // make every packed row a whole number of the kernel's blocks at an aligned
    // address. Anything else falls to the scalar swap, which is the arm that
    // made the first narrow measurement slower than the full-width path.
    uint16_t *panel = tx_buf[tx_front];
    bool aligned = pie_swap && (x%16)==0 && (cols%16)==0;
    for (int r=0;r<rows;r++) {
        uint16_t *in = pixels+r*LCD_W+x, *out = panel+r*cols;
        if (aligned) swap_pie(in, out, (unsigned)(cols*2/32));
        else swap_scalar(out, in, cols);
    }
    if (g_board_async) {
        e = tx_reap();
        if (e == ESP_OK) {
            gpio_set_level(34, 1);
            tx_pending = (spi_transaction_t){.length = bytes * 8, .tx_buffer = panel};
#ifdef KASANE_P0_BUS_PROBE
            atomic_store_explicit(&lcd_data_isr_us,0u,memory_order_relaxed);
            tx_pending.user=&lcd_data_isr_us;
#endif
            e = spi_device_queue_trans(lcd, &tx_pending, portMAX_DELAY);
            tx_inflight = (e == ESP_OK);
            tx_front ^= 1;
        }
    } else {
        e = tx_reap();
        if (e == ESP_OK) e = tx(true, panel, bytes);
    }
    next_row = -1;
    return e;
}

esp_err_t board_present_rect_sync(int x, int y, int cols, int rows, uint16_t *pixels) {
    esp_err_t e=tx_reap();
    if(e==ESP_OK) e=board_present_rect(x,y,cols,rows,pixels);
    if(e==ESP_OK) e=tx_reap();
    if(e!=ESP_OK) next_row=-1;
    return e;
}

esp_err_t board_present_sync(int y, int rows, uint16_t *pixels) {
    esp_err_t e=tx_reap();
    if(e==ESP_OK) e=board_present(y,rows,pixels);
    if(e==ESP_OK) e=tx_reap();
    if(e!=ESP_OK) next_row=-1;
    return e;
}
