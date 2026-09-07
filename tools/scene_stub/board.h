/* Host stand-in for main/hal/board.h: the three things the scene files use.
   board_rgb is the real one, copied, because the tests compare pixels. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#define LCD_W 240
#define LCD_H 135
#define STRIP_H 8
static inline uint16_t board_rgb(unsigned r, unsigned g, unsigned b) {
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}
