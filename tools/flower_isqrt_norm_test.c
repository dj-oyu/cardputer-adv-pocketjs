// Verifies main/scene/fixed_sqrt.h against sqrtf over the domain the pipeline
// uses, and reports the error in the currency that matters: relative bits.
//
//   gcc -O2 tools/flower_isqrt_norm_test.c -lm -o /tmp/isqrt && /tmp/isqrt
#include <stdio.h>
#include <math.h>
#include <stdint.h>
#include "fixed_sqrt.h"

int main(void) {
    // 1. Every 16-bit radicand: the domain of a PIE lane.
    double worst16 = 0; uint32_t at16 = 0;
    for (uint32_t x = 1; x <= 0xFFFF; x++) {
        int sh; uint32_t r = flower_isqrt_norm(x, &sh);
        double got = (double)r * ldexp(1.0, sh), want = sqrt((double)x);
        double rel = fabs(got - want) / want;
        if (rel > worst16) { worst16 = rel; at16 = x; }
    }
    printf("16-bit domain: worst relative error %.3e at x=%u (%.1f exact bits)\n",
           worst16, at16, -log2(worst16 ? worst16 : 1e-30));

    // 2. The radicand sizes the ellipsoid test actually produces: b*b - q2*c with
    //    b up to ~800 and the near-tangent hits close to zero, then the values the
    //    normal() call squares. Swept logarithmically and at random.
    double worst32 = 0; uint32_t at32 = 0;
    for (int e = 0; e < 32; e++) {
        for (int f = 1; f < 64; f++) {
            uint32_t x = (uint32_t)(((double)f / 64.0) * ldexp(1.0, e));
            if (!x) continue;
            int sh; uint32_t r = flower_isqrt_norm(x, &sh);
            double got = (double)r * ldexp(1.0, sh), want = sqrt((double)x);
            double rel = fabs(got - want) / want;
            if (rel > worst32) { worst32 = rel; at32 = x; }
        }
    }
    uint32_t s = 12345;
    for (int i = 0; i < 200000; i++) {
        s = s * 1664525u + 1013904223u;
        uint32_t x = s ? s : 1;
        int sh; uint32_t r = flower_isqrt_norm(x, &sh);
        double got = (double)r * ldexp(1.0, sh), want = sqrt((double)x);
        double rel = fabs(got - want) / want;
        if (rel > worst32) { worst32 = rel; at32 = x; }
    }
    printf("32-bit domain: worst relative error %.3e at x=%u (%.1f exact bits)\n",
           worst32, at32, -log2(worst32 ? worst32 : 1e-30));

    // 3. The scaled form, in the two places the pipeline wants it: a Q14 normal
    //    (x = dot(n,n) in Q28, so out = 14) and a Q10 depth term.
    double worstq = 0;
    for (uint32_t x = 1; x <= 0xFFFF; x++) {
        int32_t q14 = flower_isqrt_at(x, 14);
        double want = sqrt((double)x) * 16384.0;
        worstq = fmax(worstq, fabs((double)q14 - want) / want);
    }
    printf("Q14 output:    worst relative error %.3e (%.1f exact bits)\n", worstq,
           -log2(worstq ? worstq : 1e-30));

    // 4. Monotonicity of the reconstructed root, because a depth test that is not
    //    monotone in the radicand flips pixels that are not near a silhouette.
    int bad = 0;
    double prev = -1;
    for (uint32_t x = 1; x <= 0xFFFFF; x++) {
        int sh; uint32_t r = flower_isqrt_norm(x, &sh);
        double got = (double)r * ldexp(1.0, sh);
        if (got < prev - 1e-9) bad++;
        prev = got;
    }
    printf("monotone over the reconstructed root (x <= 2^20): %s (%d violations)\n",
           bad ? "NO" : "yes", bad);
    return 0;
}
