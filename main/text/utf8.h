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

// Strict validation, which utf8_decode() deliberately does not do: it never
// fails, so that a display loop cannot desynchronise. An API that accepts text
// from a program has the opposite duty — section 4 of docs/common-api.md
// refuses malformed UTF-8 and lone surrogates at a text API — and this is that
// check. Overlong forms, truncated tails, surrogates and anything past
// U+10FFFF are all rejected.
//
// pocket_storage.c and pocket_fs.c each carry a private copy of this predicate,
// written before there was a shared home for it. New code uses this one.
static inline bool utf8_valid(const char *s, size_t n) {
    const unsigned char *p=(const unsigned char *)s;
    for(size_t i=0;i<n;) {
        unsigned char c=p[i];
        size_t   extra;
        uint32_t cp;
        if(c<0x80)              { i++; continue; }
        else if((c&0xe0)==0xc0) { extra=1; cp=c&0x1fu; }
        else if((c&0xf0)==0xe0) { extra=2; cp=c&0x0fu; }
        else if((c&0xf8)==0xf0) { extra=3; cp=c&0x07u; }
        else return false;
        if(i+extra>=n) return false;
        for(size_t k=1;k<=extra;k++) {
            if((p[i+k]&0xc0)!=0x80) return false;
            cp=(cp<<6)|(uint32_t)(p[i+k]&0x3fu);
        }
        if(extra==1 && cp<0x80)    return false;
        if(extra==2 && cp<0x800)   return false;
        if(extra==3 && cp<0x10000) return false;
        if(cp>0x10ffff)            return false;
        if(cp>=0xd800 && cp<=0xdfff) return false;   // lone surrogate
        i+=extra+1;
    }
    return true;
}
