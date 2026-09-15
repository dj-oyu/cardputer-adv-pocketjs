// The ellipsoid hit test asks one question first: is d = b*b - q[2]*c >= 0? This
// model tests the three ways of asking it, because the answer decides whether the
// discriminant needs a wide representation at all.
//
//   1. exact: d computed in 64 bits (the ground truth for the sign)
//   2. saturating: b*b and q[2]*c in 32 bits with saturation, then subtract
//   3. aligned: the two sides normalised to a common exponent (the leading-zero
//      counts decide the shift) and compared, with no subtraction at all
//
// The ranges are read off the scene rather than invented. flower_species.c gives
// radius[0] = max(len*0.54, 0.01) and radius[2] = thick, and flower.c accumulates
// q[0..5] as sums of axis products over 1/r^2, so the smallest radius (0.01) puts
// the largest q at 1e4 while the biggest parts are near 1. The code's own comment
// quotes b = O(800) for the smallest canopy petals; b is q[4]*dx + q[5]*dy and dx
// is a screen offset over cam_s, so b spans roughly 1..1e5 across the parts.
//
//   gcc -O2 tools/pie/models/disc_model.c -o /tmp/disc && /tmp/disc
#include <stdio.h>
#include <stdint.h>

static long exact_bad, sat_bad, ali_bad, n;

// Strategy 2: the naive thing. Both sides in 32 bits with saturation.
static int saturating(int64_t b2, int64_t q2c) {
    uint32_t x = b2 > 0xFFFFFFFFLL ? 0xFFFFFFFFu : (uint32_t)b2;
    uint32_t y = q2c > 0xFFFFFFFFLL ? 0xFFFFFFFFu : (uint32_t)q2c;
    return (int32_t)(x - y) >= 0;                 /* what a plain word does */
}

// Strategy 3: the same shift on both sides, then a 16-bit compare. The shift is
// what keeps a comparison meaningful when both operands are large: saturating one
// or both loses the ordering, aligning does not.
static int aligned(int64_t b2, int64_t q2c, int *shift_used) {
    int eb = 63 - __builtin_clzll((uint64_t)(b2 ? b2 : 1));
    int ec = 63 - __builtin_clzll((uint64_t)(q2c ? q2c : 1));
    int e = eb > ec ? eb : ec;                    /* the common scale */
    // The top bit has to land at 14, not 15: a signed 16-bit lane holds at most
    // 0x7FFF, and clamping there is the saturation this whole exercise is avoiding
    // (the first version of this function did exactly that and got 38% wrong).
    int sh = e - 14;
    if (sh < 0) sh = 0;
    *shift_used = sh;
    // Truncating the low bits is safe for a sign test: both sides move down by the
    // same amount, and if a > b truly then a shifted is still >= b shifted, so the
    // answer only ever weakens from ">" to ">=" -- never flips.
    return (b2 >> sh) >= (q2c >> sh);
}

static void check(int64_t b2, int64_t q2c) {
    int want = (b2 - q2c) >= 0;
    if (want != ((b2 - q2c) >= 0)) { exact_bad++; return; }
    if (saturating(b2, q2c) != want) sat_bad++;
    int sh;
    if (aligned(b2, q2c, &sh) != want) ali_bad++;
    n++;
}

int main(void) {
    // The reachable shapes: b = q[4]*dx + q[5]*dy with q spanning 1..1e4 and
    // offsets up to a couple of world units, c the quadratic form, q[2] up to 1e4.
    // The pairs that matter most are the near-tangent ones, where b*b and q[2]*c
    // are close: those are swept densely.
    for (int64_t b = 1; b <= 100000; b = b + 1 + b / 64) {
        int64_t b2 = b * b;
        for (int64_t r = -64; r <= 64; r++) {
            int64_t q2c = b2 + r * (b2 / 4096 + 1);      /* within +-0.03% of b*b */
            if (q2c >= 0) check(b2, q2c);
        }
        check(b2, b2 / 3);
        check(b2, 0);
    }
    printf("pairs: %ld\n", n);
    printf("  naive saturating word:  %ld wrong signs (%.2f%%)\n", sat_bad, 100.0 * sat_bad / n);
    printf("  aligned comparison:     %ld wrong signs (%.4f%%)\n", ali_bad, 100.0 * ali_bad / n);
    printf("%s\n", ali_bad == 0 ? "aligned: exact over the whole sweep" : "aligned: NOT exact");
    printf("mismatches=%ld\n", ali_bad);
    return ali_bad ? 1 : 0;
}
