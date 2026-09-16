#include "jpfont.h"
/* Adapter tests use the real builtin font and missing-face fallback. Mapped
 * Japanese coverage is tested separately by kasane_contract/test_font.c. */
bool jpfont_bitmap(jpfont_id_t font,uint32_t cp,jpfont_bitmap_view *out){
    (void)font;(void)cp;(void)out;return false;
}
