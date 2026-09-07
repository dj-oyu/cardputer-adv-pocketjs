#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// PPT2: 3 row-compressed 4bpp bodies, 12 palettes, sparse coat overrides.
// No allocations; callers can retain this borrowed flash view indefinitely.
bool pet_pixels_valid(const uint8_t *data, size_t size);
void pet_pixels_row(const uint8_t *data, unsigned pet, unsigned y, uint16_t row[64]);
void pet_pixels_face(unsigned pet, unsigned y, unsigned mood, uint16_t row[64]);
void pet_pixels_draw(const uint8_t *data, unsigned pet, uint16_t *strip,
                     int width, int strip_y, int rows, int x, int y, unsigned divisor, unsigned mood);
