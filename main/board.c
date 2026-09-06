#include "board.h"
#include "motion.h"
#include "sound.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static spi_device_handle_t lcd;
static i2c_master_dev_handle_t keyboard;
static uint16_t wire[LCD_W * STRIP_H] __attribute__((aligned(4)));
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
uint16_t board_rgb(unsigned r, unsigned g, unsigned b) {
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}
esp_err_t board_init(void) {
    gpio_config_t g = {.pin_bit_mask = (1ULL<<33)|(1ULL<<34)|(1ULL<<38), .mode = GPIO_MODE_OUTPUT};
    ESP_ERROR_CHECK(gpio_config(&g));
    gpio_set_level(38, 0); gpio_set_level(33, 0); vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(33, 1); vTaskDelay(pdMS_TO_TICKS(120));
    spi_bus_config_t bus = {.mosi_io_num=35, .miso_io_num=-1, .sclk_io_num=36,
        .quadwp_io_num=-1, .quadhd_io_num=-1, .max_transfer_sz=sizeof(wire)};
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
    spi_device_interface_config_t dev = {.clock_speed_hz=40000000, .mode=0, .spics_io_num=37, .queue_size=1};
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &lcd));
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
    motion_init(ih);
    sound_init(ih);
    return ESP_OK;
}
board_key_t board_key(void) {
    uint8_t count=0, event=0;
    if (kread(0x03, &count) != ESP_OK || !(count & 15)) return KEY_NONE;
    if (kread(0x04, &event) != ESP_OK) return KEY_NONE;
    kwrite(0x02, 0x1f);
    if (!(event & 0x80)) return KEY_NONE;
    int code=(event & 0x7f)-1;
    if (code < 0 || code/10 >= 7 || code%10 >= 8) return KEY_NONE;
    int row=(code%10)%4, col=(code/10)*2+(code%10 >= 4);
    ESP_LOGI("key", "row=%d col=%d", row, col);
    if (row==2 && col==13) return KEY_ENTER;
    if (row==0 && col==0) return KEY_BACK;
    if (row==3 && col==10) return KEY_LEFT;
    if (row==3 && col==12) return KEY_RIGHT;
    if (row==2 && col==11) return KEY_UP;
    if (row==3 && col==11) return KEY_DOWN;
    return KEY_NONE;
}
esp_err_t board_present(int y, int rows, const uint16_t *pixels) {
    if (y<0 || rows<1 || rows>STRIP_H || y+rows>LCD_H) return ESP_ERR_INVALID_ARG;
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
    // ST7789's 240x135 visible window in landscape (MADCTL=0x60).
    uint16_t x0=40, x1=279, y0=y+53, y1=y+53+rows-1;
    uint8_t xs[]={x0>>8,x0,x1>>8,x1}, ys[]={y0>>8,y0,y1>>8,y1};
    esp_err_t e=command(0x2a,xs,4); if(e) return e;
    e=command(0x2b,ys,4); if(e) return e;
    for(int i=0;i<LCD_W*rows;i++) wire[i]=(pixels[i]<<8)|(pixels[i]>>8);
    return command(0x2c,wire,LCD_W*rows*2);
}
