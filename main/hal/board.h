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
esp_err_t board_present(int y, int rows, uint16_t *pixels);
uint16_t board_rgb(unsigned r, unsigned g, unsigned b);
void board_capture(bool enabled);

// Battery voltage at the pack, and when it was sampled. There is deliberately
// no percentage and no charging flag here: the board has no fuel gauge and no
// charger status line, and a state of charge guessed from one voltage of an
// uncharacterised cell would be a number that looks measured and is not.
typedef struct { int millivolts; int64_t time_us; } board_battery_t;
// False if the ADC or its calibration is unavailable, in which case there is no
// reading to report rather than a bad one.
bool board_battery_read(board_battery_t *out);
