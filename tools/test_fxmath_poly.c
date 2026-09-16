// Host: proves the polynomial arm inside main/scene/fxbench.c is c66f401's
// implementation, instruction for instruction, against the committed file.
//
// The bench's poly arm is a copy of the implementation f1699c3 replaced. A copy
// is only evidence if it is the same code, and there is exactly one way to check
// that without keeping the old file in the tree: compile the old file and
// compare the two arms' output bit for bit. The reference is the committed blob,
// renamed so both can be linked into one binary:
//
//   git show c66f401:main/scene/fxmath.c > /tmp/fxpoly_ref.c
//   sed -i -e 's/\bfx_core\b/fxref_core/g' -e 's/\bfx_quarter\b/fxref_quarter/g' -e 's/\bfx_bits\b/fxref_bits/g' -e 's/\bfx_frac_turns\b/fxref_frac_turns/g' -e 's/\bfx_cosf\b/fxref_cosf/g' -e 's/\bfx_sinf\b/fxref_sinf/g' -e 's/\bfx_tanf\b/fxref_tanf/g' -e 's/\bFX_STEP\b/FXREF_STEP/g' -e 's/\bFX_INV_HI\b/FXREF_INV_HI/g' -e 's/\bFX_INV_LO\b/FXREF_INV_LO/g' -e 's/\bFX_Q31\b/FXREF_Q31/g' /tmp/fxpoly_ref.c
//   gcc -O2 -Wall -Wextra -Werror -DCONFIG_POCKET_FX_BENCH=1 tools/test_fxmath_poly.c
//       /tmp/fxpoly_ref.c -I main/scene -lm -o /tmp/test_fxpoly
//
// (The sed is one line with no line continuations: a backslash ending a //
// comment is a -Wcomment warning, and this file is built with -Werror.)
//
// The rename is what a reviewer has to trust; what the test then proves is that
// the renamed file and the copy in fxbench.c answer identically over ~4.7M
// angles. Both are checked against libm in the same sweep, so the polynomial's
// 0.27 ulp (fxmath.c's own table) is visible beside the LUT's 0.96, and the
// shipping tan's reciprocal approximation beside the polynomial's division.
#include "../main/scene/fxmath.c"
#include "../main/scene/fxbench.c"
#include <math.h>
#include <stdio.h>
#include <string.h>

extern float fxref_sinf(float x);
extern float fxref_cosf(float x);
extern float fxref_tanf(float x);

static uint32_t bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static uint64_t mism_sin, mism_cos, mism_tan, calls;
static double lut_sin, lut_cos, lut_tan, poly_sin, ref_sin;

static void one(float x) {
    calls++;
    float s = sinf(x), c = cosf(x), t = tanf(x);
    if (bits(fx_poly_sinf(x)) != bits(fxref_sinf(x))) mism_sin++;
    if (bits(fx_poly_cosf(x)) != bits(fxref_cosf(x))) mism_cos++;
    if (bits(fx_poly_tanf(x)) != bits(fxref_tanf(x))) mism_tan++;
    double ds = fabs((double)fx_sinf(x) - (double)s);
    double dc = fabs((double)fx_cosf(x) - (double)c);
    double dt = fabs((double)fx_tanf(x) - (double)t);
    double ps = fabs((double)fx_poly_sinf(x) - (double)s);
    double rs = fabs((double)fxref_sinf(x) - (double)s);
    if (ds > lut_sin) lut_sin = ds;
    if (dc > lut_cos) lut_cos = dc;
    if (dt > lut_tan) lut_tan = dt;
    if (ps > poly_sin) poly_sin = ps;
    if (rs > ref_sin) ref_sin = rs;
}

int main(void) {
    // The fine grid tools/test_fxmath.c sweeps, then the regimes where the two
    // implementations' angle reductions differ: large arguments, and the raw bit
    // patterns of every float in a range.
    for (int i = 0; i < 2097153; i++) one((float)i / 131072.0f - 8.0f);
    for (int i = 0; i < 300000; i++) one((float)i * 400.0f - 6.0e7f);
    for (uint32_t b = 0x3F000000u; b < 0x41000000u; b += 7u) {
        float f; memcpy(&f, &b, 4); one(f);
    }
    printf("poly arm == c66f401's file: %llu calls, mismatches sin=%llu cos=%llu tan=%llu\n",
           (unsigned long long)calls, (unsigned long long)mism_sin,
           (unsigned long long)mism_cos, (unsigned long long)mism_tan);
    printf("worst |arm - libm| over the sweep: sin lut=%.4g poly=%.4g ref=%.4g | cos lut=%.4g | tan lut=%.4g\n",
           lut_sin, poly_sin, ref_sin, lut_cos, lut_tan);
    if (mism_sin || mism_cos || mism_tan) { printf("FXBENCH_POLY_FAIL\n"); return 1; }
    printf("FXBENCH_POLY_OK: the copy in fxbench.c is c66f401's implementation, bit for bit\n");
    return 0;
}
