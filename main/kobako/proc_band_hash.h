/* kasane.shiki commit's band signature: one entry (one segment, or a pattern
 * line's two) as 32 bits (docs/vm/qjs-observe.md sec.22). Shared with
 * tools/test_band_hash.c, which measures it against the murmur3 step it
 * replaced.
 *
 * Why not murmur3 any more: its block step is three multiplies and two
 * rotations a 32-bit word (Xtensa has no rotate), three words a segment plus
 * the finaliser, ~80 instructions an entry and about half of every STRESS
 * commit (r5sim, sec.19). Here a segment is two products on the ESP32-S3's
 * 32 x 32 -> 64 multiplier (MULL + MULUH each): x0|y0 times x1|y1 kept whole,
 * then its two halves mixed with the colour word and folded (wyhash's mum),
 * and one xorshift-multiply round at the end. Measured against murmur3
 * (tools/test_band_hash.c): the same avalanche, and collisions on lines that
 * move a little and on colour palettes at a random function's rate. Folding
 * the first product to 32 bits at once doubled those collisions; leaving out
 * the last round left a 0.066 avalanche bias.
 *
 * Why not PIE: the hash is per entry, entries are 10 bytes (5 halfwords), and
 * lanes = entries would need a 5 x 8 transpose, while folding the lanes back
 * to 32 bits stays scalar: ~23 instructions an entry that way against ~20
 * for this (estimates, sec.22).
 *
 * A mum loses its other operand when one operand is 0. The first product's
 * second operand never is: K1's halves are values no coordinate halfword
 * takes (a coordinate is -480..720: 0..720 or 65056..65535). The others are
 * 0 only by a 2^-32 chance, a collision's odds.
 */
#pragma once

#include <stdint.h>
#include "ui/kasane/ksn_shiki_frame.h"

#define PROC_BAND_K0 0x5A37C3E1u
#define PROC_BAND_K1 0x3C6E9D55u   /* halves 0x3C6E, 0x9D55: no coordinate */
#define PROC_BAND_K2 0xC2B2AE3Du
#define PROC_BAND_K3 0x7F4A7C15u

static inline __attribute__((always_inline)) uint32_t proc_band_mum(uint32_t a, uint32_t b) {
    const uint64_t p = (uint64_t)a * b;
    return (uint32_t)p ^ (uint32_t)(p >> 32);
}

static inline __attribute__((always_inline)) uint32_t proc_band_entry_hash(const ksn_proc_segment *s,
                                                                            unsigned n) {
    uint32_t h = 0x2545F491u ^ n;
    for (unsigned k = 0; k < n; k++) {
        const uint32_t a = (uint16_t)s[k].x0 | (uint32_t)(uint16_t)s[k].y0 << 16;
        const uint32_t b = (uint16_t)s[k].x1 | (uint32_t)(uint16_t)s[k].y1 << 16;
        const uint32_t c = (uint32_t)s[k].color | (uint32_t)k << 16;
        /* The first product kept whole (64 bits): folding it at once to 32
         * cost twice a random function's collisions on lines that move a
         * little (tools/test_band_hash.c). */
        const uint64_t p = (uint64_t)(a ^ h ^ PROC_BAND_K0) * (b ^ PROC_BAND_K1);
        h = proc_band_mum((uint32_t)p ^ c ^ PROC_BAND_K2, (uint32_t)(p >> 32) ^ PROC_BAND_K3);
    }
    h ^= h >> 16; h *= 0x85EBCA6Bu; h ^= h >> 13;
    return h;
}
