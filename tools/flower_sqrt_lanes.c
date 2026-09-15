// Can the restoration square root be done eight at a time on this part? The
// answer is about the LANE, not the algorithm, so this file is the arithmetic
// rather than an opinion.
//
//   gcc -O2 tools/flower_sqrt_lanes.c -o /tmp/lanes && /tmp/lanes
//
// The PIE lane is 16 bits. The recurrence the scalar helper uses is
//     t = root + bit;  m = (rem >= t);  rem -= t & m;  root = (root>>1) + (bit & m)
// and every one of those steps has a 16-bit PIE instruction behind it: VCMP.GE
// produces a full 0xFFFF mask in the lane (that is what `m` is), VADD/VSUB do
// the arithmetic, ANDQ/ORQ the masking, and the shifts are by a constant per
// step because the steps are unrolled. So the loop maps op for op -- the
// question is only whether the INPUT fits in the lane.
//
// The input is x = d * 2^2B: a number whose bit length is bits(d) + 2B, and the
// loop needs one step per two bits (ceil). A 16-bit lane therefore caps
// bits(d) + 2B at 16. Below: what that leaves for B, and that the lane version
// is bit-for-bit the scalar version for every x that fits.
#include <stdint.h>
#include <stdio.h>

// The scalar shape flower_isqrt_q() uses, over a 16-bit x (no prescale loop:
// the entry bit is chosen by the caller, which is what the lane version does).
static uint32_t restore16(uint32_t x, int steps) {
    uint32_t rem = x, root = 0, bit = 1u << (2 * (steps - 1));
    for (int i = 0; i < steps; i++, bit >>= 2) {
        uint32_t t = root + bit;
        uint32_t m = (uint32_t)0 - (uint32_t)(rem >= t);
        rem -= t & m;
        root = (root >> 1) + (bit & m);
    }
    return root;
}

// The same recurrence over eight 16-bit lanes, written the way the PIE kernel
// would be: one op per lane per step, no branches anywhere.
static void restore8(const uint16_t *x, uint16_t *out, int steps) {
    uint16_t rem[8], root[8];
    for (int i = 0; i < 8; i++) { rem[i] = x[i]; root[i] = 0; }
    uint16_t bit = (uint16_t)(1u << (2 * (steps - 1)));
    for (int s = 0; s < steps; s++, bit >>= 2) {
        for (int i = 0; i < 8; i++) {                 // <- one PIE op each, all 8 lanes at once
            uint16_t t = (uint16_t)(root[i] + bit);
            uint16_t m = (uint16_t)(rem[i] >= t ? 0xFFFFu : 0u);   // VCMP.GE.S16
            rem[i] = (uint16_t)(rem[i] - (t & m));                 // VSUB + ANDQ
            root[i] = (uint16_t)((root[i] >> 1) + (bit & m));      // shift + ANDQ + VADD
        }
    }
    for (int i = 0; i < 8; i++) out[i] = root[i];
}

int main(void) {
    // 1. The lane version IS the scalar version, for every x a 16-bit lane holds.
    int bad = 0;
    for (uint32_t x = 1; x <= 0xFFFF; x++) {
        uint16_t one = (uint16_t)x, got = 0;
        restore8(&one, &got, 8);
        if (got != restore16(x, 8)) { if (bad < 3) printf("  mismatch x=%u\n", x); bad++; }
    }
    printf("A. 8-lane vs scalar, all x in [1,65535], 8 steps: %s (%d mismatches)\n",
           bad ? "MISMATCH" : "identical", bad);

    // 2. Cost, counted honestly: a step is
    //      t = root + bit        VADD
    //      m = (rem >= t)        VCMP.GE  -> 0xFFFF a lane
    //      rem -= t & m          ANDQ + VSUB
    //      root = (root>>1)+(bit&m)  SRL + ANDQ + VADD
    //    = 7 lane-ops, and `bit` is a constant because the steps are unrolled.
    printf("B. 7 lane-ops a step (t=ADD, m=VCMP, AND+SUB, SRL+AND+ADD)\n");
    printf("   16-bit lanes (8 roots a register), 8 steps:  %3d PIE insns -> %4.1f insns a root\n",
           8 * 7, 8 * 7 / 8.0);
    printf("   32-bit lanes (4 roots a register), 16 steps: %3d PIE insns -> %4.1f insns a root\n",
           16 * 7, 16 * 7 / 4.0);
    printf("   scalar restore, 7 insns a step x 8..11 steps:  %d..%d insns a root\n", 7 * 8, 7 * 11);

    // 3. What the lane width costs. A lane of W bits holds the radicand, and the
    //    integer square root keeps half of its bits: W/2 significant bits of
    //    root. Those have to cover log2(root) integer bits plus B fractional
    //    ones, so B = W/2 - ceil(log2(root)). Exponent/mantissa packing does not
    //    change this -- it only decides WHERE the radicand sits in the lane -- so
    //    the lane width is the ceiling on the precision.
    printf("C. significant bits out of a lane, and the B they allow\n");
    printf("   %-10s %-14s %-14s %-10s %s\n", "lane", "sig. bits out", "root <= 1", "root <= 2", "root <= 4");
    struct { const char *name; int bits; } lanes[] = {{"16-bit", 16}, {"32-bit", 32}};
    for (unsigned i = 0; i < 2; i++) {
        int sig = lanes[i].bits / 2;
        printf("   %-10s %-14d B<=%-11d B<=%-9d B<=%d\n", lanes[i].name, sig, sig, sig - 1, sig - 2);
    }
    printf("   (the scalar helper runs at B=14: 14 + 2..3 = 16..17 significant bits,\n");
    printf("    which is what the frame comparison showed is needed for no visible speck;\n");
    printf("    B=8 with ~10 significant bits is where the white pixel appears)\n");
    // 4. The scalar total, for the ratio
    printf("D. ratio: 16-bit lanes, 56 insns for 8 roots against ~63 a root scalar = %.1fx fewer;\n",
           63.0 / 7.0);
    printf("   32-bit lanes, 112 insns for 4 roots = %.1f insns a root = %.1fx fewer -- and that is the\n",
           112 / 4.0, 63.0 / (112 / 4.0));
    printf("   arrangement that holds B=14. Packing, unpacking and the float conversion around the\n");
    printf("   lanes are not in these counts, so the real ratio is worse than the arithmetic.\n");
    return 0;
}
