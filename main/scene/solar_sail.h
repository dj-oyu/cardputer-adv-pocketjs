#pragma once
#include <stdint.h>
void solar_sail_prepare(float dt, int tilt_x, int tilt_y);
void solar_sail_draw(uint16_t *pixels, int y, int height);
const char *solar_sail_target(void);
const char *solar_sail_time_label(void);
