// Host: gcc -O2 -Wall -Wextra -Werror tools/test_fxmath.c -lm -o .cache/test_fxmath
//
// The fixed-point trig (main/scene/fxmath.c) is what replaced libm's sinf, cosf
// and tanf in the scenes and in the MP3 filter's coefficient build, so that
// three compiler_builtins sections could fall out of the image. Nothing about
// that change is visible on the device if it is wrong: a wrong table entry moves
// the picture by a level, and an angle off by a fraction of a degree moves it
// by a pixel, which is exactly the kind of error the eye forgives on this panel.
//
// The implementation is a Q31 quadrant-cosine table with a three-point Lagrange
// between its entries, so the accuracy is the *table's*, and the things worth
// checking are therefore:
//
//  1. the table itself, entry by entry against libm's cosine -- it is generated
//     (tools/gen_fx_lut.py) and committed, and a stray digit would be silent;
//  2. sin and cos swept against the true value over five different regimes (fine
//     grid, scene clocks, table and quadrant boundaries, large arguments, bit
//     patterns), held to the ~1 ulp the Q31 entries can carry. That floor does
//     not move with the table size: this table measures 0.96 ulp over the sweep
//     below, a 512-interval quadratic 0.95, and a 2,275-interval *linear* table
//     (9,104 bytes against 872 here) 0.99. The bound below is a floor check, not
//     a nicety. The degree-5 polynomial this replaced ran at 0.27 ulp; the pixel
//     cost of moving to the floor is measured in the dump harnesses, not here
//     (14 pixels of 84,661,200, all one shading step);
//  3. the divergence from libm is *counted* rather than bounded, because the
//     number that matters for the picture is how often the last bit differs;
//  4. the two callers whose output is an integer are decided exhaustively --
//     wave.c's level_slope over every tilt the IMU path can produce, and the
//     MP3 filter's 32 coefficients at the three rates the header accepts, where
//     a different coefficient would change decoded audio rather than a pixel.
#include "../main/scene/fxmath.c"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

// ulp of magnitude 1: the resolution of a float result whatever its size, so the
// error is reported in a unit that does not jump around as the value passes zero.
#define ULP1 1.1920929e-07
// One and a quarter of them. The measured worst case over the sweep below is
// 0.965, and both numbers are printed, so a regression shows up as a number
// before it shows up as a failure.
#define ULP1_LIMIT 1.25
static const double TABLE_H = (3.14159265358979323846 / 2) / FX_LUT_INTERVALS;

static long total, differ;
static double worst, worst_vs;
static float worst_at, worst_vs_at;

static void one_sin(float x) {
    double t = sin((double)x);
    float got = fx_sinf(x), lib = sinf(x);
    total++;
    double e = fabs((double)got - t);
    if (e > worst) { worst = e; worst_at = x; }
    if (got != lib) {
        differ++;
        double d = fabs((double)got - (double)lib);
        if (d > worst_vs) { worst_vs = d; worst_vs_at = x; }
    }
}
static void one_cos(float x) {
    double t = cos((double)x);
    float got = fx_cosf(x), lib = cosf(x);
    total++;
    double e = fabs((double)got - t);
    if (e > worst) { worst = e; worst_at = x; }
    if (got != lib) {
        differ++;
        double d = fabs((double)got - (double)lib);
        if (d > worst_vs) { worst_vs = d; worst_vs_at = x; }
    }
}
static long total_all;
static double worst_all;
static void report(const char *what) {
    printf("%-30s %9ld calls  differ from libm %7ld (%6.4f%%)  max error %.3f ulp1"
           "  max |fx-libm| %.3f ulp1\n", what, total, differ,
           100.0 * differ / total, worst / ULP1, worst_vs / ULP1);
    // The Q31 entries carry a half LSB each and the evaluation rounds through
    // float, so ~1 ulp is where any table of this width lands; the figure printed
    // above is the one to watch. The limit is a floor check: a dropped entry, a
    // wrong quadrant or a mis-scaled fraction all blow past it by 100x.
    assert(worst <= ULP1 * ULP1_LIMIT);
    total_all += total;
    if (worst > worst_all) worst_all = worst;
    total = differ = 0; worst = worst_vs = 0;
}

// The table is the part that cannot be checked by eye, and it is also the part a
// typo would corrupt silently -- and it is generated now, so what is checked is
// that the committed header is what tools/gen_fx_lut.py would write: entry m is
// round(cos((m-1)*h)*2^31) in the shifted layout the three-point Lagrange reads,
// and cos(m*h) unshifted when a linear table is being built.
static void table_matches_libm(void) {
#if FX_LUT_SHIFT != 31
    printf("FAIL: the shipped table is Q31; a Q15 table's own LSB is 256 ulp1\n");
    assert(FX_LUT_SHIFT == 31);
#endif
    const int shift = FX_LUT_ORDER == 2 ? -1 : 0;
    int checked = 0;
    for (int m = 0; m < FX_LUT_ENTRIES; m++) {
        int k = m + shift;
        double v = cos(k * TABLE_H) * 2147483648.0;
        if (v > 2147483647.0) v = 2147483647.0;        // entry 0 is the clamp
        int want = (int)floor(v + 0.5);
        int got = fx_quarter[m];
        if (want != got) {
            printf("FAIL: table[%d] = %d, cos(%d*pi/2/%d) fits %d\n", m, got, k, FX_LUT_INTERVALS, want);
            assert(want == got);
        }
        checked++;
    }
    assert(checked == FX_LUT_ENTRIES);
    // The clamped entry is the one at angle zero -- index 0 unshifted, index 1
    // in the shifted layout -- and the last entry is the quadrant's end, which is
    // exactly zero.
    assert(fx_quarter[FX_LUT_ORDER == 2 ? 1 : 0] == 2147483647);
    assert(fx_quarter[FX_LUT_ENTRIES - 1] == 0);
    printf("table: %d entries agree with round(cos((m%+d)*pi/2/%d)*2^31), both ends clamped\n",
           checked, shift, FX_LUT_INTERVALS);
}

// wave.c: level_slope = (int)(tanf(tilt_x/256.0f) * 256), and motion.c clamps
// tilt_x to +-180. Every value the firmware can produce is in this loop.
static void wave_level_slope(void) {
    long bad = 0;
    for (int t = -180; t <= 180; t++) {
        float x = t / 256.0f;
        if ((int)(tanf(x) * 256) != (int)(fx_tanf(x) * 256)) bad++;
    }
    assert(bad == 0);
    printf("wave: level_slope identical for all 361 reachable tilt values "
           "(tan at the clamp: %.6f)\n", (double)fx_tanf(180 / 256.0f));
    // Beyond the clamp the tangent's poles make the integer meaningless; the
    // count is reported so that a future caller with a wider tilt is warned.
    long wide = 0;
    for (int t = -4096; t <= 4096; t++)
        if ((int)(tanf(t / 256.0f) * 256) != (int)(fx_tanf(t / 256.0f) * 256)) wide++;
    printf("      outside the clamp (unreachable): %ld/8193 differ\n", wide);
}

// pocket/mp3_decode.c builds its 32-tap windowed sinc once per file. The three
// rates the header accepts are the only ones that reach it (a lower rate skips
// the FIR), and here they are compared coefficient by coefficient: a change of
// one LSB of 16384 is -84 dBFS on that tap, but it would change the decoded
// sample hash, so the bar is equality.
static void mp3_filter(void) {
    static const unsigned rates[] = { 32000, 44100, 48000 };
    for (unsigned r = 0; r < 3; r++) {
        int16_t lib[32], fx[32];
        for (int use_fx = 0; use_fx < 2; use_fx++) {
            float taps[32], sum = 0;
            float cutoff = 10800.0f / (float)rates[r];
            for (unsigned i = 0; i < 32; i++) {
                float x = (float)i - 15.5f;
                float a = 6.28318530718f * cutoff * x;
                float b = 6.28318530718f * i / 31.0f;
                float s = use_fx ? fx_sinf(a) : sinf(a);
                float c = use_fx ? fx_cosf(b) : cosf(b);
                taps[i] = s / (3.14159265359f * x) * (0.54f - 0.46f * c);
                sum += taps[i];
            }
            int16_t *out = use_fx ? fx : lib;
            int acc = 0;
            for (unsigned i = 0; i < 32; i++) {
                out[i] = (int16_t)lroundf(taps[i] * 16384.0f / sum);
                acc += out[i];
            }
            out[15] += (int16_t)(16384 - acc);
        }
        for (unsigned i = 0; i < 32; i++)
            if (lib[i] != fx[i]) { printf("FAIL: %u Hz tap %u %d != %d\n", rates[r], i, lib[i], fx[i]); }
        assert(memcmp(lib, fx, sizeof lib) == 0);
        printf("mp3: 32 coefficients identical at %u Hz\n", rates[r]);
    }
}

int main(void) {
    table_matches_libm();

    for (int i = -(1 << 20); i <= (1 << 20); i++) one_sin(i * 7.62939453125e-06f);
    report("grid 2^-17 over +-8 rad");
    for (int i = 0; i < 7200; i++) {
        float e = i * (1.0f/30);
        one_sin(e * 0.1f); one_sin(e * 0.6f); one_sin(e * 0.7f); one_sin(e * 0.8f);
        one_sin(e * 0.3f); one_sin(e * 0.4f); one_sin(e * 0.5f); one_sin(e * 2.7f);
        one_sin(e * 0.2f);
        one_cos(e * 0.1f); one_cos(e * 0.6f);
    }
    report("scene clocks (7200 frames)");
    for (int j = -8192; j <= 8192; j++) {
        double base = j * TABLE_H;
        for (int k = -4; k <= 4; k++) {
            float x = (float)(base + k * TABLE_H * 1e-7);
            one_sin(x); one_cos(x);
        }
    }
    for (int j = -64; j <= 64; j++)
        for (int k = -3; k <= 3; k++) {
            float x = (float)(j * (M_PI/2) + k * 1e-6);
            one_sin(x); one_cos(x);
        }
    report("table/quadrant boundaries");
    for (int i = 0; i < 400000; i++) {
        one_sin((float)(i * 0.25));
        one_sin(1e5f + i * 0.1f);
        one_cos((float)(i * 0.25));
    }
    report("large arguments (1e5 rad)");
    {
        union { float f; uint32_t u; } v;
        for (uint32_t b = 0x3F000000u; b < 0x41000000u; b += 97) {
            v.u = b;               one_sin(v.f); one_cos(v.f);
            v.u = b | 0x80000000u; one_sin(v.f);
        }
    }
    report("float bit patterns 0.5..8");

    // Degenerate inputs: a scene hands these in when a clock has not started or
    // a tilt is exactly level, and they were the cases the earlier reduction got
    // wrong. With a table the ends are exact by construction: cos(0) reads entry
    // 0 (the clamped 1.0f) and a quarter turn's sine reads the table's zero.
    assert(fx_sinf(0.0f) == 0.0f && fx_cosf(0.0f) == 1.0f);
    assert(fx_sinf(-0.0f) == 0.0f && fx_cosf(-0.0f) == 1.0f);
    assert(fx_sinf((float)(M_PI/2)) == 1.0f);
    assert(fx_sinf((float)(-M_PI/2)) == -1.0f);
    assert(fabsf(fx_cosf((float)(M_PI/2))) < ULP1 && fabsf(fx_cosf((float)-(M_PI/2))) < ULP1);
    assert(fx_sinf(1e-45f) == 0.0f && fx_cosf(1e-45f) == 1.0f);
    assert(fx_sinf(INFINITY) == 0.0f && fx_cosf(NAN) == 1.0f);
    printf("degenerate inputs: 0, -0, both quarter turns, denormal, inf, nan\n");

    wave_level_slope();
    mp3_filter();
    printf("FXMATH_OK: %d-entry table exact, %.3f ulp1 worst error over %ld swept "
           "calls (Q31 floor), level_slope and the MP3 coefficients bit-identical\n",
           FX_LUT_ENTRIES, worst_all / ULP1, total_all);
    return 0;
}
