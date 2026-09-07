#pragma once
#include <stdint.h>

// OCEAN + STARS. The signatures are scene_ops_t's exactly (scene/scene.h), so
// the table points straight at them with no adapter in between; `variant` is
// ignored, as it is by every scene but the flower.
void ocean_prepare(float dt, int tilt_x, int tilt_y, unsigned variant);
uint32_t ocean_draw(uint16_t *strip, int y, int height);
void ocean_overlay(uint16_t *strip, int y, int height);
