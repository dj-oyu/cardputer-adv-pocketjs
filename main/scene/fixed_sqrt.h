// A square root with no float on either side. Part of the fixed-point pipeline:
// the caller hands over a bare integer and gets back a 16-bit root plus the
// power of two it belongs to, sqrt(x) == root * 2^shift.
//
// Why this shape, and not the Q2B-in/QB-out one flower_isqrt_q() is built on.
// The ellipsoid test computes d = b*b - q[2]*c where b = q[4]*dx + q[5]*dy is
// O(800) for the smallest petals and the interesting hits are the ones where d
// is O(0): near tangency the subtraction cancels, so the radicand needs close to
// 40 bits -- 20 bits of relative precision on a value whose fixed point would
// have to be scaled for 800^2. That is more than a 32-bit word holds, which is
// the arithmetic reason plain fixed point cannot carry this term, and it is why
// a single sqrt at the end of a float pipeline measures like sqrtf() itself: the
// conversions in and out are soft-float calls and the loop was never the cost.
// Normalising by the radicand's own leading bit is what puts the dynamic range
// back in the exponent where it costs nothing, and it leaves the mantissa small
// enough to sit in a 16-bit PIE lane, eight roots at a time.
//
// Accuracy: 16 significant bits, so a relative error below 2^-15 (the float
// routine it replaces is good to 2^-24). tools/flower_isqrt_norm_test.c sweeps
// the domain and prints the worst case that actually occurs.
#ifndef FLOWER_FIXED_SQRT_H
#define FLOWER_FIXED_SQRT_H

#include <stdint.h>

// sqrt(x) == flower_isqrt_norm(x,&shift) * 2^shift. x = 0 gives (0,0).
// The mantissa keeps 15 significant bits whatever x is: the normalisation puts
// the radicand's leading bit at position 29 or 30, shifting *up* for small x
// (where a bare integer root would lose everything below it -- floor(sqrt(3)) is
// 1 against 1.732) and down for large. The shift is even, so half of it is the
// root's exponent.
static inline uint32_t flower_isqrt_norm(uint32_t x, int *shift) {
    if (!x) { *shift = 0; return 0; }
    int e = 31 - __builtin_clz(x);          // NSA on this part: one instruction
    int up = 29 - e;
    int drop = up > 0 ? -(up & ~1) : (e - 29) & ~1;   // even, either direction
    uint32_t m = drop > 0 ? (x >> drop) : (x << -drop);
    uint32_t r = 0, rem = m, bit = 1u << ((31 - __builtin_clz(m)) & ~1u);
    for (; bit; bit >>= 2) {
        uint32_t t = r + bit;
        uint32_t k = (uint32_t)0 - (uint32_t)(rem >= t);   // VCMP gives this mask a lane
        rem -= t & k;
        r = (r >> 1) + (bit & k);
    }
    *shift = drop / 2;
    return r;
}

// The same root placed at a caller-chosen scale: returns round(sqrt(x) * 2^out).
// out = 14 gives a Q14 unit-vector norm, which is what a fixed-point normal()
// wants. Saturates rather than wrapping when the answer does not fit, so a caller
// that asks for more precision than the result has gets a large number, not a
// negative one.
static inline int32_t flower_isqrt_at(uint32_t x, int out) {
    int sh;
    uint32_t r = flower_isqrt_norm(x, &sh);   // sqrt(x) == r * 2^sh
    int k = out + sh;                         // ... so it is r * 2^(out-sh+sh) at scale out
    if (k >= 0) {
        if (k > 16 || (uint64_t)r << k > 0x7FFFFFFF) return 0x7FFFFFFF;
        return (int32_t)(r << k);
    }
    return (-k > 31) ? 0 : (int32_t)(r >> (-k));
}

#endif
