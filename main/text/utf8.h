#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// UTF-8, in one place. It was written out four times before this, and one of
// those copies had lost its four-byte branch — the kind of difference that
// only shows up on the one input nobody tried.

// A continuation byte: part of a character, never the start of one.
static inline bool utf8_is_cont(char c) {
    return ((unsigned char)c & 0xc0) == 0x80;
}

// The scalar at `i`, with `*adv` set to the bytes it used. Malformed input
// advances by one and reads as U+FFFD, so a broken string can never
// desynchronise a caller's loop. Anything outside the BMP reads as U+FFFD too:
// the glyph tables here index by uint16.
static inline uint32_t utf8_decode(const char *s, size_t len, size_t i, size_t *adv) {
    unsigned char c=(unsigned char)s[i];
    if(c<0x80)                    { *adv=1; return c; }
    if((c&0xe0)==0xc0 && i+1<len) { *adv=2; return ((c&0x1fu)<<6)|(s[i+1]&0x3f); }
    if((c&0xf0)==0xe0 && i+2<len) { *adv=3; return ((c&0x0fu)<<12)|((s[i+1]&0x3f)<<6)|(s[i+2]&0x3f); }
    if((c&0xf8)==0xf0 && i+3<len) { *adv=4; return 0xfffd; }
    *adv=1; return 0xfffd;
}
