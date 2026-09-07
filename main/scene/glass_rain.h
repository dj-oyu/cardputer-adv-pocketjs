#pragma once
#include <stdint.h>

// Screen-space background pass, called before the existing XMB overlay.
void glass_rain_prepare(float dt, uint32_t seed);
void glass_rain_draw(uint16_t *pixels, int y, int height);
