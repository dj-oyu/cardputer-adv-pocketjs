#pragma once
#include <stdint.h>
#include "board.h"

// Drawing shared by every screen that composes one strip at a time.
//
// Each of them had its own copy of these two, identical to the byte in the
// case of the latin face. One task draws and one strip is live at a time — the
// same reason board_strip() can be a single buffer — so the current strip is
// module state here rather than an argument threaded through every call.

// Call at the top of each strip, before drawing into it.
void paint_begin(uint16_t *strip, int strip_y, int strip_h);

// The 5x7 latin face, for labels a Japanese font need not carry. Characters
// outside 0x20-0x7E draw as '?'.
void paint_ascii(int x, int y, const char *s, uint16_t colour);

void paint_fill(int x, int y, int w, int h, uint16_t colour);
