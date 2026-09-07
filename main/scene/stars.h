#pragma once
#include <stdbool.h>
#include <stdint.h>

// The drifting, twinkling star field the two sky scenes share.
//
// It holds no state of its own. Both scenes borrow their memory from
// scene_mem and only one of them is ever live, so a star array owned here
// would be a third claim on a block that has one owner at a time. The caller
// keeps the array inside its own block and passes it in, which also means this
// file needs no rebuild flag: the storage belongs to whoever borrowed it.
#define STARS_N 36
typedef struct { int x, y; uint16_t color; } star_t;

// Positions and colours for this frame. `clock` is the scene's own animation
// time in seconds. `parallax` shifts three depth layers against the tilt, which
// LEVEL WAVE does and OCEAN + STARS does not -- a parameter of the scene rather
// than a second test of which menu row is selected.
void stars_prepare(star_t *stars, double clock, int tilt_x, int tilt_y, bool parallax);

// Both writers take the strip and the absolute row `y` its first line is, and
// clip to `height` rows themselves.
//
// Points, with a cross of dimmer pixels on every seventh star so the bright
// ones read as stars rather than as dead pixels. What the ocean draws.
void stars_draw_points(const star_t *stars, uint16_t *strip, int y, int height);
// Three layers blended into what is already there: the near ones soft discs,
// the far ones a single dimmed dot. What the level wave draws.
void stars_draw_layers(const star_t *stars, uint16_t *strip, int y, int height);
