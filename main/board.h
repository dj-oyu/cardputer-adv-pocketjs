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
bool board_key_event(board_keyevent_t *out);
// The one strip buffer every screen draws into; board_present consumes it.
uint16_t *board_strip(void);
esp_err_t board_present(int y, int rows, uint16_t *pixels);
uint16_t board_rgb(unsigned r, unsigned g, unsigned b);
void board_capture(bool enabled);
