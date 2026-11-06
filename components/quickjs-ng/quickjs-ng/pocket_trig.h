/* POCKET: sin/cos/tan from Shiki's quarter-wave table instead of libm's
 * double kernels (docs/vm/qjs-observe.md sec.18). Not part of upstream
 * quickjs-ng; it lives here because every build that compiles quickjs.c (the
 * firmware, r5sim, vmtest, the host harnesses) already has this directory on
 * its include path.
 *
 * Why: Math.sin/cos/tan went through libm's double sin/cos/tan, whose
 * argument reduction and kernels are all soft-float on the ESP32-S3 (~600
 * instructions a call), and which keep ~5.4 KB of flash linked
 * (__kernel_rem_pio2, __rem_pio2, __kernel_sin/cos/tan, their tables).
 * ECMAScript leaves the precision of these functions to the implementation;
 * the owner accepted a lower one. Everything in the firmware that called the
 * double functions (QuickJS, pocket_derby.c, solar_sail.c) calls these
 * instead, so DERBY's C series and its JS oracle still agree bit for bit.
 *
 * How: the argument becomes a phase, x * 2^32 / (2 pi) modulo 2^32, taken from
 * the double's bits (exact modular reduction of that product for any
 * magnitude); the quarter-wave table (round(sin(k pi / 512) * 65536), k =
 * 0..256, the same values as Shiki's sec.4.4 table) is interpolated linearly
 * in Q30. Error <= ~1.3e-5 absolute (interpolation 4.7e-6 + table rounding
 * 7.6e-6). Exact where the specification is exact: NaN and +-Infinity give
 * NaN, +-0 gives +-0 (sin, tan), cos(+-0) = 1; |x| < 2^-10 gives x for sin and
 * tan (error < 2e-10).
 *
 * The table and the functions are weak, so each translation unit may include
 * this header and the image keeps one copy. ksn_shiki.c makes the firmware's
 * copy the Shiki table itself (a strong alias), and
 * tools/check_pocket_trig_table.py checks that the two literals agree. */
#pragma once

#include <stdint.h>
#include <string.h>

#define POCKET_TRIG_WEAK __attribute__((weak))

POCKET_TRIG_WEAK const int32_t pocket_sin_q16[257] = {
    0,402,804,1206,1608,2010,2412,2814,
    3216,3617,4019,4420,4821,5222,5623,6023,
    6424,6824,7224,7623,8022,8421,8820,9218,
    9616,10014,10411,10808,11204,11600,11996,12391,
    12785,13180,13573,13966,14359,14751,15143,15534,
    15924,16314,16703,17091,17479,17867,18253,18639,
    19024,19409,19792,20175,20557,20939,21320,21699,
    22078,22457,22834,23210,23586,23961,24335,24708,
    25080,25451,25821,26190,26558,26925,27291,27656,
    28020,28383,28745,29106,29466,29824,30182,30538,
    30893,31248,31600,31952,32303,32652,33000,33347,
    33692,34037,34380,34721,35062,35401,35738,36075,
    36410,36744,37076,37407,37736,38064,38391,38716,
    39040,39362,39683,40002,40320,40636,40951,41264,
    41576,41886,42194,42501,42806,43110,43412,43713,
    44011,44308,44604,44898,45190,45480,45769,46056,
    46341,46624,46906,47186,47464,47741,48015,48288,
    48559,48828,49095,49361,49624,49886,50146,50404,
    50660,50914,51166,51417,51665,51911,52156,52398,
    52639,52878,53114,53349,53581,53812,54040,54267,
    54491,54714,54934,55152,55368,55582,55794,56004,
    56212,56418,56621,56823,57022,57219,57414,57607,
    57798,57986,58172,58356,58538,58718,58896,59071,
    59244,59415,59583,59750,59914,60075,60235,60392,
    60547,60700,60851,60999,61145,61288,61429,61568,
    61705,61839,61971,62101,62228,62353,62476,62596,
    62714,62830,62943,63054,63162,63268,63372,63473,
    63572,63668,63763,63854,63944,64031,64115,64197,
    64277,64354,64429,64501,64571,64639,64704,64766,
    64827,64884,64940,64993,65043,65091,65137,65180,
    65220,65259,65294,65328,65358,65387,65413,65436,
    65457,65476,65492,65505,65516,65525,65531,65535,
    65536
};

/* The phase of |x|: |x| * 2^32 / (2 pi), truncated, modulo 2^32. The product is
 * one double multiply; the reduction reads its bits, in 32-bit halves (-Os
 * GCC on Xtensa makes a variable 64-bit shift a libgcc call). */
static inline uint32_t pocket_trig_phase(double x) {
    const double q = x * 683565275.57643158978;   /* 2^32 / (2 pi) */
    uint64_t bits;
    memcpy(&bits, &q, sizeof bits);
    const uint32_t hi = (uint32_t)(bits >> 32), lo = (uint32_t)bits;
    const int e = (int)((hi >> 20) & 0x7FFu) - 1075;   /* q = m * 2^e */
    const uint32_t mhi = (hi & 0xFFFFFu) | 0x100000u, mlo = lo;   /* m, 53 bits */
    uint32_t p;
    if (e >= 32)
        p = 0;                        /* every bit of m is above 2^32 */
    else if (e >= 0)
        p = mlo << e;                 /* the low 32 bits of m * 2^e */
    else if (e > -32)
        p = (mlo >> -e) | (mhi << (32 + e));
    else if (e > -53)
        p = mhi >> (-e - 32);
    else
        p = 0;
    return p;   /* of |x|: callers apply the sign, so sin and tan are odd, cos even */
}

/* sin of a phase, in Q30 (2^30 = 1.0). */
static inline int32_t pocket_trig_sin_q30(uint32_t p) {
    uint32_t w = (p >> 8) & 0x3FFFFFu;        /* within the quarter, 2^22 a quarter */
    if (p & 0x40000000u) w = 0x400000u - w;   /* the falling quarters mirror */
    const uint32_t i = w >> 14, t = w & 0x3FFFu;
    int32_t v = pocket_sin_q16[i] << 14;
    if (i < 256) v += (pocket_sin_q16[i + 1] - pocket_sin_q16[i]) * (int32_t)t;
    return (p & 0x80000000u) ? -v : v;
}

/* Q30 to double: exact (|v| <= 2^30), and the scaling is an exponent change. */
static inline double pocket_trig_q30_to_double(int32_t v) {
    double d = (double)v;
    if (v != 0) {
        uint64_t bits;
        memcpy(&bits, &d, sizeof bits);
        bits -= (uint64_t)30 << 52;
        memcpy(&d, &bits, sizeof d);
    }
    return d;
}

/* exponent field 0x7FF: NaN or infinity */
static inline int pocket_trig_not_finite(double x) {
    uint64_t bits;
    memcpy(&bits, &x, sizeof bits);
    return ((uint32_t)(bits >> 52) & 0x7FFu) == 0x7FFu;
}
static inline int pocket_trig_negative(double x) {
    uint64_t bits;
    memcpy(&bits, &x, sizeof bits);
    return (int)(bits >> 63);
}
static inline int pocket_trig_tiny(double x) {   /* |x| < 2^-10, including +-0 */
    uint64_t bits;
    memcpy(&bits, &x, sizeof bits);
    return ((uint32_t)(bits >> 52) & 0x7FFu) < 1023u - 10u;
}

/* The shared bodies, out of line (weak: one copy in the image). */
POCKET_TRIG_WEAK uint32_t pocket_trig_phase_of(double x) { return pocket_trig_phase(x); }
POCKET_TRIG_WEAK double pocket_trig_sin_of_phase(uint32_t p) {
    return pocket_trig_q30_to_double(pocket_trig_sin_q30(p));
}

POCKET_TRIG_WEAK double pocket_sin(double x) {
    if (pocket_trig_not_finite(x)) return x - x;   /* NaN for NaN and +-Infinity */
    if (pocket_trig_tiny(x)) return x;
    const double v = pocket_trig_sin_of_phase(pocket_trig_phase_of(x));
    return pocket_trig_negative(x) ? -v : v;
}

POCKET_TRIG_WEAK double pocket_cos(double x) {
    if (pocket_trig_not_finite(x)) return x - x;
    return pocket_trig_sin_of_phase(pocket_trig_phase_of(x) + 0x40000000u);
}

POCKET_TRIG_WEAK double pocket_tan(double x) {
    if (pocket_trig_not_finite(x)) return x - x;
    if (pocket_trig_tiny(x)) return x;
    const uint32_t p = pocket_trig_phase_of(x);
    const int32_t s = pocket_trig_sin_q30(p);
    int32_t c = pocket_trig_sin_q30(p + 0x40000000u);
    /* the phase can land on cos's zero, which no finite double does: answer
     * the steepest finite slope instead of an infinity */
    if (c == 0) c = (p & 0x40000000u) ? -1 : 1;
    const double v = (double)s / (double)c;
    return pocket_trig_negative(x) ? -v : v;
}
