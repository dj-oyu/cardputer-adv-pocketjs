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
// `(float)v * 2^-31` is exactly the rounding the float result needs, and the
// fixmath table is fine enough that this conversion, not the table, is the
// error term. The angle reduction is done in integers: `(float)v` and every
// float multiply are soft-float calls on this core, and a reduction through
// `(int32_t)(x * 1/(2pi))` would have been one, so the fraction of a turn is
// taken from the float's own bits instead.
#include "fxmath.h"
#include <stdint.h>

// cos over one quadrant, TABLE[j] = round(cos(j*h) * 2^31) for h = pi/128, so
// TABLE[0] is one and TABLE[64] is zero. sin(j*h) is the same table read
// backwards -- TABLE[64-j] -- because the two are a quarter turn apart, which
// is what makes one table serve both.
//
// TABLE[0] is 2^31-1 rather than 2^31: the exact value does not fit in int32,
// and 2^31-1 converts to 1.0f exactly anyway.
static const int32_t fx_quarter[65] = {
    2147483647, 2146836866, 2144896910, 2141664948,
    2137142927, 2131333572, 2124240380, 2115867626,
    2106220352, 2095304370, 2083126254, 2069693342,
    2055013723, 2039096241, 2021950484, 2003586779,
    1984016189, 1963250501, 1941302225, 1918184581,
    1893911494, 1868497586, 1841958164, 1814309216,
    1785567396, 1755750017, 1724875040, 1692961062,
    1660027308, 1626093616, 1591180426, 1555308768,
    1518500250, 1480777044, 1442161874, 1402678000,
    1362349204, 1321199781, 1279254516, 1236538675,
    1193077991, 1148898640, 1104027237, 1058490808,
    1012316784,  965532978,  918167572,  870249095,
     821806413,  772868706,  723465451,  673626408,
     623381598,  572761285,  521795963,  470516330,
     418953276,  367137861,  315101295,  262874923,
     210490206,  157978697,  105372028,   52701887,
             0,
};

// One table step in Q31 radians: round(pi/128 * 2^31). The correction's error
// term is the seventh power of this over 5040, i.e. 1e-15, so degree 5 is
// already past what Q31 can hold.
#define FX_STEP   52707179
// 1/(2pi) as 2^31*FX_INV_HI + FX_INV_LO, split because the product with a
// 24-bit mantissa is 53 bits wide and one integer cannot hold 62 bits. Two
// terms leave 2^-60 of relative error, which at the largest angle a scene can
// reach is 1e-9 radians.
#define FX_INV_HI 341782637
#define FX_INV_LO 1692680576
#define FX_Q31    0x1p-31f

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

// cos and sin of x, Q31.
static void fx_core(float x, int32_t *cout, int32_t *sout) {
    uint32_t b = fx_bits(x);
    int neg = (int)(b >> 31);
    uint32_t u = fx_frac_turns(b);
    int quad = (int)(u >> 30);              // which quarter turn
    uint32_t o = u & 0x3FFFFFFFu;           // the rest of it, Q30 of a quarter
    int j = (int)(o >> 24);                 // table index, 0..63
    uint32_t f = o & 0xFFFFFFu;             // and the remainder, Q24 of a step
    int32_t d = (int32_t)(((int64_t)f * FX_STEP) >> 24);   // Q31 radians past the point
    // cos(d) and sin(d) about the table point: 1 - d^2/2 + d^4/24 and d - d^3/6.
    // d^5/120 is under half an LSB of Q31 and is dropped. The two divisions are
    // by reciprocal multiplies: an int64 division here would be a call to
    // compiler_builtins' __divdi3 -- 223 instructions, and a new claim on the
    // very member this file exists to let go of. 43691 = ceil(2^18/6) =
    // ceil(2^20/24), and the identity floor(x/6) == (x*43691)>>18 is exact for
    // every x < 131,072: d^3 peaks at 31,750 and d^4 at 779 over the whole
    // domain of d, so both are three orders of magnitude inside that.
    int64_t d2 = ((int64_t)d * d) >> 31;
    int64_t d3 = (d2 * d) >> 31;
    int64_t cd = (int64_t)(1u << 31) - (d2 >> 1) + (((d2 * d2) >> 31) * 43691 >> 20);
    int64_t sd = (int64_t)d - ((d3 * 43691) >> 18);
    int64_t cj = fx_quarter[j], sj = fx_quarter[64 - j];
    // cos(a+d) and sin(a+d) from the table point: the rotation, in Q62 and
    // rounded back to Q31. The products are bounded by 2^31 * 2^31, so the sum
    // fits int64 with a factor of two to spare.
    int64_t cq = ((cj * cd - sj * sd) + (int64_t)(1u << 30)) >> 31;
    int64_t sq = ((sj * cd + cj * sd) + (int64_t)(1u << 30)) >> 31;
    // The quadrant's reflection, applied before narrowing: a quarter turn's sine
    // is exactly 2^31, which int32 cannot hold. Clamped to 2^31-1, which is 1.0f
    // exactly, so the clamp costs nothing.
    int64_t cc = (quad & 1) ? sq : cq;
    int64_t ss = (quad & 1) ? cq : sq;
    if (quad == 1 || quad == 2) cc = -cc;
    if (quad >= 2) ss = -ss;
    if (neg) ss = -ss;
    if (cc > (int64_t)0x7FFFFFFF) cc = (int64_t)0x7FFFFFFF;
    if (ss > (int64_t)0x7FFFFFFF) ss = (int64_t)0x7FFFFFFF;
    *cout = (int32_t)cc;
    *sout = (int32_t)ss;
}

float fx_cosf(float x) { int32_t c, s; fx_core(x, &c, &s); return (float)c * FX_Q31; }
float fx_sinf(float x) { int32_t c, s; fx_core(x, &c, &s); return (float)s * FX_Q31; }
// The tangent is the one the middle of the picture has no use for, so it stays
// a division of the two above rather than a third table.
float fx_tanf(float x) { int32_t c, s; fx_core(x, &c, &s); return (float)s / (float)c; }
