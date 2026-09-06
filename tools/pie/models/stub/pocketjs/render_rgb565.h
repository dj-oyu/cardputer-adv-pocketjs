#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct { uint32_t x, y, width, height; } pocketjs_rgb565_rect_t;
typedef bool (*pocketjs_rgb565_fill_fn)(void *, uint16_t *, size_t, uint32_t, uint32_t, pocketjs_rgb565_rect_t, uint16_t);
typedef bool (*pocketjs_rgb565_blend_fn)(void *, uint16_t *, size_t, uint32_t, uint32_t, const uint8_t *, size_t, pocketjs_rgb565_rect_t, uint8_t, uint8_t, uint8_t, uint8_t);
typedef bool (*pocketjs_rgb565_srm_fn)(void *, uint16_t *, size_t, uint32_t, uint32_t, const uint8_t *, size_t, uint32_t, uint32_t, pocketjs_rgb565_rect_t, pocketjs_rgb565_rect_t, uint32_t, bool, bool);
typedef struct { size_t struct_size; void *user_data; pocketjs_rgb565_fill_fn fill_rgb565; pocketjs_rgb565_blend_fn blend_a8_rgb565; pocketjs_rgb565_srm_fn srm_psm5650_rgb565; } pocketjs_rgb565_accelerator_t;
