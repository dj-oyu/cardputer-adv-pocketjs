#pragma once
// Where the menu puts its text, as a rule rather than as a sample.
//
// This exists because an assertion was written from one screenshot. The garden's
// birth-and-death trace was placed at rows 112..132 on the grounds that "the
// HUD's labels sit at rows 37, 69 and 89 and nothing is below 100" -- true of
// the state that was looked at, and false in general, because the XMB scroll
// animates every item's row. The board put SKK PRACTICE across rows 108..125,
// straight through the trace.
//
// The rule below is the whole of it, and two facts fall out of it that no
// sampling would have found:
//
//   * DURING A SCROLL NO ROW IS SAFE. item_y is continuous in the animated
//     position and spans well past both edges of the screen, and label() has no
//     clamp and no opacity cut-off (the faintest an item gets is 0.30), so for
//     every row there is some instant of some scroll that puts text on it.
//     "A band the menu never occupies" does not exist.
//
//   * AT REST THE ROWS ARE DISCRETE. The positions settle on integers, so the
//     deltas are integers and the item rows are 69+57k for k<0 and 69+41k for
//     k>=0: ..., 12, 69, 110, 151, ... Fourteen rows tall at scale 2. With the
//     category line at 37 and the detail at 89, the occupied set at rest is
//     12..25, 37..43, 69..82, 89..95, 110..123 -- and the free bands are
//     0..11, 26..36, 44..68, 83..88, 96..109 and 124..134.
//
// So a decoration can be placed where the menu does not rest, and has to accept
// being crossed while the menu moves. That is a weaker claim than the one that
// was asserted, and it is the true one. tools/test_menu_rows.c derives the
// bands from this header rather than from a screenshot.
#define MENU_CATEGORY_Y 37
#define MENU_DETAIL_Y   89
#define MENU_ITEM_SCALE 2
// One glyph row per pixel of font, scaled: text() draws 7*scale rows from y.
#define MENU_TEXT_ROWS(scale) (7*(scale))
// Leave space for the category rail between the previous and focused item.
static inline float menu_item_y(float delta) {
    return delta<0?69+57*delta:69+41*delta;
}
