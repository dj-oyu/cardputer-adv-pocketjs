// Per-call cost of the fixed-point trig, for the three implementations that
// have stood in the scenes' call sites, measured in ONE binary.
//
// The question this exists to answer cannot be asked the usual way. The scene
// numbers this project trusts come from flipping a switch once per 60-frame
// window inside one binary, and this change has no switch to flip: fx_sinf is
// not a variant of a call, it *is* the call, and the implementation it replaced
// (the degree-5 polynomial of c66f401) is not in the tree any more. Two builds
// cannot be compared either -- the same code moves up to 15% between them from
// instruction-cache placement alone (CLAUDE.md), and the frame effect here is
// expected to be ~0.05 ms, i.e. under any window's noise floor. What *can* be
// compared in one binary is the cost of one call, so that is what this measures:
// the three arms are timed in the same run, interleaved round by round, and the
// empty loop that carries the load and the accumulate is subtracted out.
//
// The arms:
//
//   lut    the shipping implementation, fxmath.c's Q31 218-entry table with a
//          three-point Lagrange between entries (fx_core, 137 instructions)
//   poly   c66f401's Q31 65-entry table with the degree-5 correction, copied
//          here verbatim (renamed) so the call it makes is the call the scenes
//          made before f1699c3 (194 instructions). tools/test_fxmath_poly.c
//          proves this copy bit-identical to the committed c66f401 file.
//   libm   sinf/cosf/tanf, the compiler_builtins routines the whole exercise
//          removed from the image: linking them back here costs 6,789 bytes and
//          is the point of the measurement, not a regression (this file is
//          compiled only in a bench build, CONFIG_POCKET_FX_BENCH).
//
// Gated by CONFIG_POCKET_FX_BENCH, which sdkconfig.defaults leaves off: on, the
// bench adds ~1 kB of table and code plus whatever libm pulls in. The device
// part is behind ESP_PLATFORM as well, so the host harness can include this file
// for its arms alone (gcc -DCONFIG_POCKET_FX_BENCH=1).
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#if CONFIG_POCKET_FX_BENCH

#include <stdint.h>

// ---------------------------------------------------------------------------
// The polynomial arm: main/scene/fxmath.c as of c66f401 ("三角関数を固定小数へ"),
// copied with every file-scope name prefixed so it can live beside the current
// implementation. Only the names changed; the table, the step, the reciprocal
// constants and every instruction of the arithmetic are as committed there.
//
//   git show c66f401:main/scene/fxmath.c
//
// Why it is here rather than referenced: c66f401's file was replaced by
// f1699c3, so the only copy of it is in history, and a bench that reads its arm
// out of `git show` cannot be run on the device at all.
#define FXP_Q31 0x1p-31f
#define FXP_STEP 52707179
#define FXP_INV_HI 341782637
#define FXP_INV_LO 1692680576

static const int32_t fxp_quarter[65] = {
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

static uint32_t fxp_bits(float x) {
    union { float f; uint32_t u; } v;
    v.f = x;
    return v.u;
}

static uint32_t fxp_frac_turns(uint32_t b) {
    uint32_t mag = b & 0x7FFFFFFFu;
    unsigned e = (mag >> 23) & 0xFFu;
    if (e == 0xFFu) return 0;
    uint32_t m;
    int shift;
    if (e) { m = (mag & 0x7FFFFFu) | 0x800000u; shift = (int)e - 127; }
    else   { m = mag & 0x7FFFFFu; shift = -126; }
    uint64_t P = (uint64_t)m * FXP_INV_HI + (((uint64_t)m * FXP_INV_LO) >> 31);
    int k = shift - 22;
    if (k >= 0) return k >= 32 ? 0u : (uint32_t)((uint32_t)P << k);
    int s = -k;
    uint32_t lo = (uint32_t)P, hi = (uint32_t)(P >> 32);
    if (s < 32) return (lo >> s) | (hi << (32 - s));
    if (s < 54) return hi >> (s - 32);
    return 0;
}

// noinline in both arms, because the call is the thing being measured: fx_sinf
// is a function in another translation unit, so a scene's call is a call, and an
// arm the compiler inlined into the timing loop would be timed as a loop body
// instead.
static __attribute__((noinline)) void fxp_core(float x, int32_t *cout, int32_t *sout) {
    uint32_t b = fxp_bits(x);
    int neg = (int)(b >> 31);
    uint32_t u = fxp_frac_turns(b);
    int quad = (int)(u >> 30);
    uint32_t o = u & 0x3FFFFFFFu;
    int j = (int)(o >> 24);
    uint32_t f = o & 0xFFFFFFu;
    int32_t d = (int32_t)(((int64_t)f * FXP_STEP) >> 24);
    int64_t d2 = ((int64_t)d * d) >> 31;
    int64_t d3 = (d2 * d) >> 31;
    int64_t cd = (int64_t)(1u << 31) - (d2 >> 1) + (((d2 * d2) >> 31) * 43691 >> 20);
    int64_t sd = (int64_t)d - ((d3 * 43691) >> 18);
    int64_t cj = fxp_quarter[j], sj = fxp_quarter[64 - j];
    int64_t cq = ((cj * cd - sj * sd) + (int64_t)(1u << 30)) >> 31;
    int64_t sq = ((sj * cd + cj * sd) + (int64_t)(1u << 30)) >> 31;
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

__attribute__((noinline)) float fx_poly_cosf(float x) { int32_t c, s; fxp_core(x, &c, &s); return (float)c * FXP_Q31; }
__attribute__((noinline)) float fx_poly_sinf(float x) { int32_t c, s; fxp_core(x, &c, &s); return (float)s * FXP_Q31; }
// The tangent this arm shipped with: a soft-float division of the two above.
// The shipping arm's fx_tanf replaced that division with the reciprocal
// approximation (7482063), so the pair of tan arms measures that change too.
__attribute__((noinline)) float fx_poly_tanf(float x) { int32_t c, s; fxp_core(x, &c, &s); return (float)s / (float)c; }

#ifdef ESP_PLATFORM

#include "esp_cpu.h"
#include "esp_log.h"
#include "fxmath.h"
#include <math.h>

// The angle list is the input set, and it is a table rather than an expression of
// the loop index because a computed angle would put its own arithmetic inside
// the timed loop (a soft-float multiply per call, the very thing the arms are
// about). It covers what the scenes actually ask: every scene clock is under a
// hundred radians (solar_sail's orbit, stars' rotation), so the interesting range
// is a couple of turns, and the large entries (1e5, 1e7) exercise the angle
// reduction where the two implementations differ most.
#define FXBENCH_ANGLES 64
#define FXBENCH_N 20000
#define FXBENCH_REPEATS 15

static const float fxbench_angles[FXBENCH_ANGLES] = {
    0.0f, 1e-09f, -1e-09f, 0.00390625f,
    -0.0078125f, 0.1f, -0.25f, 0.5f,
    -0.75f, 1.0f, -1.25f, 1.5f,
    -1.75f, 2.0f, -2.5f, 3.0f,
    -3.14159265f, 3.5f, -4.0f, 4.5f,
    -5.0f, 5.5f, -6.0f, 6.2831853f,
    -6.2831853f, 7.0f, -7.5f, 8.0f,
    -9.0f, 10.0f, -12.0f, 15.0f,
    -20.0f, 31.0f, -45.0f, 60.0f,
    -90.0f, 120.0f, -180.0f, 271.0f,
    -360.0f, 720.0f, -1000.0f, 4096.0f,
    -4096.0f, 65536.0f, -65536.0f, 100000.0f,
    -100000.0f, 1000000.0f, -1000000.0f, 10000000.0f,
    -10000000.0f, 123.456f, -789.012f, 0.0009765625f,
    -0.0009765625f, 101.0f, -101.0f, 2.5f,
    -2.5f, 19.739f, -0.5f, 0.7853982f,
};

// Every arm's result goes through this, so no arm can be dead-code eliminated
// and no arm's cost includes a float accumulate: the union read is the float's
// own bits, and the accumulator is an integer.
static volatile uint32_t fxbench_sink;

#define FXBENCH_ARM(NAME, EXPR)                                            \
    static uint32_t fxbench_##NAME(void) {                                 \
        uint32_t acc = 0;                                                  \
        uint32_t c0 = esp_cpu_get_cycle_count();                           \
        for (int i = 0; i < FXBENCH_N; i++) {                              \
            float x = fxbench_angles[i & (FXBENCH_ANGLES - 1)];            \
            union { float f; uint32_t u; } v;                              \
            v.f = (EXPR);                                                  \
            acc ^= v.u;                                                    \
        }                                                                  \
        uint32_t c1 = esp_cpu_get_cycle_count();                           \
        fxbench_sink = acc;                                                \
        return c1 - c0;                                                    \
    }

FXBENCH_ARM(empty, x)
FXBENCH_ARM(sin_lut, fx_sinf(x))
FXBENCH_ARM(cos_lut, fx_cosf(x))
FXBENCH_ARM(tan_lut, fx_tanf(x))
FXBENCH_ARM(sin_poly, fx_poly_sinf(x))
FXBENCH_ARM(cos_poly, fx_poly_cosf(x))
FXBENCH_ARM(tan_poly, fx_poly_tanf(x))
FXBENCH_ARM(sin_libm, sinf(x))
FXBENCH_ARM(cos_libm, cosf(x))
FXBENCH_ARM(tan_libm, tanf(x))

typedef uint32_t (*fxbench_fn_t)(void);
typedef struct { const char *name; fxbench_fn_t fn; } fxbench_arm_t;

static const fxbench_arm_t fxbench_arms[] = {
    {"empty", fxbench_empty},
    {"sin_lut", fxbench_sin_lut},   {"cos_lut", fxbench_cos_lut},   {"tan_lut", fxbench_tan_lut},
    {"sin_poly", fxbench_sin_poly}, {"cos_poly", fxbench_cos_poly}, {"tan_poly", fxbench_tan_poly},
    {"sin_libm", fxbench_sin_libm}, {"cos_libm", fxbench_cos_libm}, {"tan_libm", fxbench_tan_libm},
};
#define FXBENCH_ARMS (sizeof(fxbench_arms)/sizeof(fxbench_arms[0]))

static uint32_t fxbench_median(uint32_t *a, int n) {
    for (int i = 1; i < n; i++) { uint32_t v = a[i]; int j = i - 1; while (j >= 0 && a[j] > v) { a[j+1] = a[j]; j--; } a[j+1] = v; }
    return (n % 2) ? a[n/2] : (a[n/2-1] + a[n/2]) / 2;
}

// The two arms that are supposed to agree do not have to agree exactly here --
// the point of this bench is cost -- but an arm that is *wrong* would make the
// cost meaningless, so the divergence from libm is printed beside it. The
// shipping table's own worst case is 0.957 ulp of magnitude 1 (tools/
// test_fxmath.c), and the polynomial's is 0.27; a copy-paste error in the poly
// arm would show up here as a much larger number.
static void fxbench_accuracy(void) {
    float dl = 0.0f, dp = 0.0f;
    for (int i = 0; i < 65536; i++) {
        // +-8 radians, the range the scene clocks live in, at a step (2^-12) far
        // finer than either table's spacing.
        float x = (float)(i - 32768) * 0.000244140625f;
        float s = sinf(x);
        float a = fabsf(fx_sinf(x) - s);
        float b = fabsf(fx_poly_sinf(x) - s);
        if (a > dl) dl = a;
        if (b > dp) dp = b;
    }
    ESP_LOGI("fxbench", "FXBENCH_ERR vs_libm_sinf max_abs lut=%.9g poly=%.9g over 65536 angles", (double)dl, (double)dp);
}

void fxbench_run(void) {
    uint32_t cy[FXBENCH_ARMS][FXBENCH_REPEATS];
    // Round-robin rather than arm-by-arm: the board's clock and the caches drift
    // over the second this takes, and interleaving puts every arm under the same
    // drift. The loop counts only, no timing, are in the empty arm.
    for (int r = 0; r < FXBENCH_REPEATS; r++)
        for (unsigned a = 0; a < FXBENCH_ARMS; a++)
            cy[a][r] = fxbench_arms[a].fn();

    uint32_t empty = fxbench_median(cy[0], FXBENCH_REPEATS);
    ESP_LOGI("fxbench", "FXBENCH_STATIC n=%d repeats=%d angles=%d cpu_mhz=240 arms=%u",
             FXBENCH_N, FXBENCH_REPEATS, FXBENCH_ANGLES, (unsigned)FXBENCH_ARMS);
    ESP_LOGI("fxbench", "FXBENCH_LOOP arm=empty median_cycles_per_call=%u max_cycles_per_call=%u",
             (unsigned)(empty / FXBENCH_N), (unsigned)(cy[0][FXBENCH_REPEATS-1] / FXBENCH_N));
    for (unsigned a = 1; a < FXBENCH_ARMS; a++) {
        uint32_t med = fxbench_median(cy[a], FXBENCH_REPEATS);
        uint32_t worst = 0;
        for (int r = 0; r < FXBENCH_REPEATS; r++) if (cy[a][r] > worst) worst = cy[a][r];
        // Per call with the loop's own cost taken out, so the number is the call
        // and not the harness. 1000/240 ns per cycle, done once at the end.
        uint32_t per = med / FXBENCH_N - empty / FXBENCH_N;
        uint32_t permax = worst / FXBENCH_N - empty / FXBENCH_N;
        ESP_LOGI("fxbench", "FXBENCH arm=%s per_call_cycles=%u max=%u ns=%u",
                 fxbench_arms[a].name, (unsigned)per, (unsigned)permax, (unsigned)((per * 1000) / 240));
    }
    fxbench_accuracy();
}

#endif /* ESP_PLATFORM */
#endif /* CONFIG_POCKET_FX_BENCH */
