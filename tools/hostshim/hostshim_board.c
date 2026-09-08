// The two lines of board.c a surface test needs, without the panel, the
// keyboard or the fonts that tools/hostshim/hostshim.c brings with it.
//
// tools/test_pocket_capture.c draws the recording indicator into an array and
// checks it is there, which needs a colour and nothing else; hostshim.c would
// have dragged in the font atlas, the source store and the IME behind it.
#include "board.h"

uint16_t board_rgb(unsigned r, unsigned g, unsigned b) {
    return (uint16_t)(((r&0xf8)<<8)|((g&0xfc)<<3)|(b>>3));
}
