// Fixed-point sine, cosine and tangent, for the callers that were reaching a
// soft-float libm routine to turn an angle into one number.
//
// Why this exists is a size question rather than a speed one: sinf/cosf/tanf are
// Rust compiler_builtins sections on this part (2,470 + 2,394 + 1,925 bytes,
// measured on this branch), kept alive by nothing but our own C calls, because
// the scenes and the MP3 filter are the only places in the image that ask. The
// alternative to a table is a few hundred bytes of code; this is the table.
//
// The representation is Q31 (one in 2^31), which is what a single-precision
// result can use and no more: converting the Q31 value to float through
// `(float)v * 2^-31` is exactly the rounding the float result needs. The angle
// reduction is done in integers: `(float)v` and every float multiply are
// soft-float calls on this core, and a reduction through `(int32_t)(x*1/(2pi))`
// would have been one, so the fraction of a turn is taken from the float's own
// bits instead.
//
// The table is one quadrant of the cosine, FX_LUT_INTERVALS intervals wide, and
// the value between its entries comes from a three-point Lagrange through the
// entry either side: no polynomial, and no per-entry correction table. sin is
// the same table read backwards -- sin(j*h + t*h) = cos((N-j-1 + 1-t)*h) --
// because the two are a quarter turn apart. The table itself is
// tools/gen_fx_lut.py's output and is committed beside this file.
//
// What the two orders buy, measured with tools/test_fxmath.c's sweep (worst
// error against the true value, in ulp of magnitude 1, and the .rodata the
// generated table takes):
//
//   linear     64 intervals   631.9 ulp  ( 65 entries,  260 B)
//   linear    512 intervals    10.3 ulp  (513 entries, 2052 B)
//   linear    1024 intervals    2.9 ulp  (1025 entries, 4100 B)
//   linear    2275 intervals    0.99 ulp (2276 entries, 9104 B)
//   quadratic   64 intervals     8.0 ulp  ( 66 entries,  264 B)
//   quadratic  216 intervals     0.97 ulp (218 entries,  872 B)
//   quadratic  512 intervals     0.95 ulp (514 entries, 2056 B)
//   the degree-5 polynomial this replaces ran at 0.27 ulp with 65 entries.
//
// The quadratic is what makes the table small: a straight line needs ten times
// the entries to reach the same place (2275 intervals, 9,104 B, for the 0.99 ulp
// the quadratic gets from 218 entries at 872 B). And the ~0.96 ulp both land on
// is a *floor* rather than a slope -- it does not move when the table gets
// finer, because it is the half-LSB rounding of the Q31 entries themselves.
// Q15 does not have this shape at all: its own least significant bit is 256 ulp,
// which is what a Q15 table measures at every size.
//
// So the choice this file makes is "the smallest table that reaches the floor"
// rather than "0.27 ulp": the polynomial's extra three digits cost 57 more
// instructions per call (194 against 137, real build, -Os) and are invisible on
// this panel -- see fxmath.h for the picture that was measured.
#include "fxmath.h"
#include <stdint.h>
#include "fx_lut_gen.h"

// Q31 (int32) or Q15 (int16) as tools/gen_fx_lut.py wrote it.
#if FX_LUT_SHIFT == 31
#define FX_LUT_SCALE 0x1p-31f
#else
#define FX_LUT_SCALE 0x1p-15f
#endif

// 1/(2pi) as 2^31*FX_INV_HI + FX_INV_LO, split because the product with a
// 24-bit mantissa is 53 bits wide and one integer cannot hold 62 bits. Two
// terms leave 2^-60 of relative error, which at the largest angle a scene can
// reach is 1e-9 radians.
#define FX_INV_HI 341782637
#define FX_INV_LO 1692680576

static uint32_t fx_bits(float x) {
    // Not a float-to-int conversion: on this core that would be a soft-float
    // call, which is the thing being removed.
    union { float f; uint32_t u; } v;
    v.f = x;
    return v.u;
}

// The fraction of a turn of |x|, Q32, where 0x40000000 is a quarter turn.
//
// x = m * 2^(e-23) with m a 24-bit mantissa, so |x|/(2pi) * 2^31 is
// P = m * (2^31/(2pi)) and the wanted fraction is P * 2^(e-22) truncated to 32
// bits. A left shift wraps in that arithmetic for free; a right shift, which is
// the case for every angle under 4.2e6 radians, is taken through the two halves
// of P so that no 64-bit variable shift (a library call) is needed.
static uint32_t fx_frac_turns(uint32_t b) {
    uint32_t mag = b & 0x7FFFFFFFu;
    unsigned e = (mag >> 23) & 0xFFu;
    if (e == 0xFFu) return 0;                       // NaN or infinity
    uint32_t m;
    int shift;
    if (e) { m = (mag & 0x7FFFFFu) | 0x800000u; shift = (int)e - 127; }
    else   { m = mag & 0x7FFFFFu; shift = -126; }   // denormal: the fraction is zero
    uint64_t P = (uint64_t)m * FX_INV_HI + (((uint64_t)m * FX_INV_LO) >> 31);
    int k = shift - 22;
    if (k >= 0) return k >= 32 ? 0u : (uint32_t)((uint32_t)P << k);
    int s = -k;
    uint32_t lo = (uint32_t)P, hi = (uint32_t)(P >> 32);
    if (s < 32) return (lo >> s) | (hi << (32 - s));
    if (s < 54) return hi >> (s - 32);
    return 0;                                       // under a denormal angle
}

// One table entry as a float. The int-to-float convert is a single FPU
// instruction on this part; doing the whole interpolation in int64 instead would
// be a 62-bit product per output.
static inline float fx_at(unsigned m) { return (float)fx_quarter[m] * FX_LUT_SCALE; }

#if FX_LUT_ORDER >= 2
// Three-point Lagrange through the nodes (m-1, a), (m, b), (m+1, c) at m+u:
// b + u*(c-a)/2 + u^2*((a+c)/2 - b). The error term is the third power of the
// step, so this is the quadratic correction that buys two more digits than the
// straight line through the same pair of entries.
static inline float fx_quad(float a, float b, float c, float u) {
    float half = (c - a) * 0.5f;
    float curv = (a + c) * 0.5f - b;
    return b + u * half + (u * u) * curv;
}
#endif

// cos and sin of x, in float.
//
// The interpolation runs in float rather than in fixed point: this part has a
// single-precision FPU, so the converts, the subtract, the multiply and the add
// are a handful of instructions, while the same interpolation in int64 would be
// a 62-bit product per output. The float's own 24-bit mantissa is wider than the
// interpolation error at every table size here, which is what makes that safe
// rather than merely cheap.
static void fx_core(float x, float *cout, float *sout) {
    uint32_t b = fx_bits(x);
    int neg = (int)(b >> 31);               // sin is odd, cos is even
    uint32_t u = fx_frac_turns(b);
    int quad = (int)(u >> 30);              // which quarter turn
    uint32_t o = u & 0x3FFFFFFFu;           // the rest of it, Q30 of a quarter
    // Interval index and the fraction across it, in one multiply: o < 2^30 and
    // FX_LUT_INTERVALS < 2^16, so the product fits 64 bits and the split is
    // exact. A power-of-two table turns this into a shift and a mask.
    uint64_t q = (uint64_t)o * FX_LUT_INTERVALS;
    unsigned j = (unsigned)(q >> 30);
    float t = (float)(int32_t)((uint32_t)q & 0x3FFFFFFFu) * 0x1p-30f;
    // sin(j*h + t*h) = cos((N-j-1 + 1-t)*h): the same table, the neighbour pair
    // the other way round, and the fraction measured from the other end.
    unsigned k = FX_LUT_INTERVALS - 1u - j;
#if FX_LUT_ORDER == 1
    float c0 = fx_at(j), c1 = fx_at(j + 1);
    float c = c0 + (c1 - c0) * t;
    // s1 + (s0-s1)*t is the same line as s0 + (s1-s0)*(1-t), without ever
    // forming 1-t: at t=0 it is the entry at k+1, at t=1 the one at k.
    float s0 = fx_at(k), s1 = fx_at(k + 1);
    float s = s1 + (s0 - s1) * t;
#else
    // The cosine's nodes j-1, j, j+1 are entries j, j+1, j+2 of the shifted
    // table; the sine's are the same three around k, with the fraction 1-t.
    float c = fx_quad(fx_at(j), fx_at(j + 1), fx_at(j + 2), t);
    float s = fx_quad(fx_at(k), fx_at(k + 1), fx_at(k + 2), 1.0f - t);
#endif
    // The quadrant's reflection. Both ends of an interval are inside the
    // quadrant, so nothing here can leave [-1,1] and no clamp is needed.
    float cc = (quad & 1) ? s : c;
    float ss = (quad & 1) ? c : s;
    if (quad == 1 || quad == 2) cc = -cc;
    if (quad >= 2) ss = -ss;
    if (neg) ss = -ss;
    *cout = cc;
    *sout = ss;
}

float fx_cosf(float x) { float c, s; fx_core(x, &c, &s); return c; }
float fx_sinf(float x) { float c, s; fx_core(x, &c, &s); return s; }

// The tangent is the one the middle of the picture has no use for (only
// scene/wave.c's horizon asks), and on this core s/c is a call into
// compiler_builtins' __divsf3 -- the one symbol this file used to leave
// undefined. The reciprocal is built here instead, from a bit-trick seed: the
// exponent field inverted gives 1/|c| to within 5%, and each Newton step squares
// the error, so three steps are past the float's own 2^-24 (5% -> 2.5e-3 ->
// 6e-6 -> 4e-11). A reciprocal *table* was the other candidate and is worse than
// it sounds: 1/cos has a pole at the far end of the quadrant, where no table
// holds it, so the same seeding would be needed anyway to cover the angles the
// table cannot.
static inline float fx_rcp(float c) {
    union { float f; uint32_t u; } v;
    v.f = c;
    uint32_t m = v.u & 0x7FFFFFFFu;      // |c| as bits
    uint32_t s = v.u ^ m;                // its sign bit, and 0 for c = 0
    v.u = 0x7EF127EAu - m;               // seed: 1/|c| to within 5%
    float r = v.f;
    v.u = m;
    float a = v.f;                       // |c| as a float
    r = r * (2.0f - a * r);
    r = r * (2.0f - a * r);
    r = r * (2.0f - a * r);
    v.f = r; v.u ^= s;
    return v.f;
}
float fx_tanf(float x) { float c, s; fx_core(x, &c, &s); return s * fx_rcp(c); }
