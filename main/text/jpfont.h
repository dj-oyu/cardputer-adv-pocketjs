#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "board.h"

// The 1bpp Japanese faces, read straight out of the mmap'd jp_font partition.
// Nothing is cached in SRAM: a 12x12 cell is 24 bytes, so the flash cache's
// 64-byte lines already hold three glyphs per fill.
//
// Two of them, because 33 px of console strip holds four 8 px lines but only
// two 12 px ones. Misaki carries no latin — its own is 3 px wide, thinner than
// the 5x7 face the panels draw ASCII with — so a caller using SMALL keeps
// drawing anything below U+0080 itself.
typedef enum {
    JPFONT_TEXT = 0,   // shinonome 12x12, with 6x12 ASCII
    JPFONT_SMALL,      // misaki 8x8, full-width only
    JPFONT_COUNT
} jpfont_id_t;

// Loads every face it finds. Returns true when at least one is there.
bool jpfont_init(void);

bool jpfont_ready(jpfont_id_t font);
unsigned jpfont_cell_w(jpfont_id_t font);
unsigned jpfont_cell_h(jpfont_id_t font);
unsigned jpfont_baseline(jpfont_id_t font);

// True when the face has a glyph for this codepoint (not the tofu box).
bool jpfont_has(jpfont_id_t font, uint32_t codepoint);

// Expands one glyph into `out` as cell_w*cell_h alpha bytes, 0 or 255, which
// is the shape PocketJS's font atlas wants. An unmapped codepoint gives the
// tofu box. Returns the advance in px, or 0 when the face is missing.
unsigned jpfont_glyph(jpfont_id_t font, uint32_t codepoint, uint8_t *out);

// Advance of one UTF-8 character in px, and the bytes it consumed.
unsigned jpfont_advance(jpfont_id_t font, const char *s, size_t len, size_t i,
                        size_t *adv);

// Width of a whole UTF-8 run in px.
unsigned jpfont_width(jpfont_id_t font, const char *s, size_t len);

// Draw a UTF-8 run into one RGB565 strip. `strip_y` is the strip's top row in
// screen space and `rows` its height; `y` is the text cell's top row, also in
// screen space, so a glyph straddling two strips draws its visible part in
// each. Returns the pen x after the run.
int jpfont_draw(jpfont_id_t font, uint16_t *pixels, int strip_y, int rows,
                int x, int y, const char *s, size_t len, uint16_t colour);

// The same, refusing to write outside [x0,x1). A horizontally scrolled column
// starts its pen left of its own left edge, so the first glyph is usually a
// partial one and would otherwise land on whatever the column is inset from --
// in the editor, the line numbers. The clip is a parameter rather than module
// state because this face is drawn from the Rust UI core as well as from the
// screens, and a clip left set by one of them would follow the other.
int jpfont_draw_clip(jpfont_id_t font, uint16_t *pixels, int strip_y, int rows,
                     int x, int y, const char *s, size_t len, uint16_t colour,
                     int x0, int x1);
