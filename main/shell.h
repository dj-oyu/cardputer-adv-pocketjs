#pragma once
#include "board.h"
void shell_init(void);
bool shell_key(board_key_t key);
unsigned shell_app(void);   // which Apps entry Enter would launch
void shell_draw(const char *error, unsigned phase);
void shell_change_background(int direction);

// Draws one ocean row both ways and reports the largest disagreement per
// channel, so a vector implementation is checked against the scalar one before
// it is allowed near the screen. Returns the worst channel difference found
// across the row, or -1 when there is no vector implementation to compare.
int shell_ocean_selftest(void);
