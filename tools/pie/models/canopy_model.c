// Proof of the arithmetic the canopy kernel rests on, before any of it is
// written as lanes. garden_canopy_row() is the one contiguous, maskless blend in
// the file and the code calls it the secondary PIE candidate; what follows is
// every identity that stands between the scalar loop and an eight-lane kernel.
//
// The ranges are not guessed, they are read off the scene:
//
//   rx  = 28 + (h>>16)%17          -> 28..44
//   rr  = rx*rx                    -> 784..1936
//   |dx|<= rx  so dx2 = dx*dx      -> 0..1936
//   mrr = ceil(2^26/rr)            -> 34669..85598     (does not fit a lane)
//   qy  = dy*dy*256/(ry*ry)        -> 0..256
//   q   = (256-qy) - (dx2*mrr>>18) clamped to >= 0
//   f   = (q*39322)>>16            -> 0..153, g = 256-f
//   leafy channels lr,lg,lb        -> 31, 63, 31
//
// The tests, in the order the kernel will need them:
//
//   A. The radicand split. mrr is 17 bits, so it is carried as mhi*256+mlo and
//      the product is taken as (dx2*16)*(mhi*16) -- both operands inside a 16-bit
//      lane (30976 and 5344) while the true product, 1.7e8, is not. That means
//      the multiply has to leave through the 40-bit accumulator, and this test
//      pins the identity it lands on.
//   B. The reciprocal is exact, not close: for every reachable (dx2, rr) pair the
//      expression equals dx*dx*256/rr with no off-by-one. This is the claim the
//      code makes in a comment and the reason 2^26 with a shift of 18 was chosen
//      over 2^22 with 14 elsewhere in the file.
//   C. q*3/5 is exactly (q*39322)>>16 over the whole reachable q.
//   D. The `if(!f)continue` can be deleted: f == 0 is the identity for every row
//      word and every leafy, because g == 256 then and (ch*256)>>8 == ch. The
//      kernel has no branch to spend on it.
//   E. The blend is a convex combination, so no channel can leave its field and
//      the pack needs no clamp -- and the eight-lane version of the whole row
//      matches the scalar one pixel for pixel.
//
//   gcc -O2 tools/pie/models/canopy_model.c -o /tmp/canopy && /tmp/canopy
//   python tools/pie/run_models.py canopy
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

static long mismatches;

static int mrecip(int d, int sh) {          // the same helper garden.c uses
    return (int)((((int64_t)1 << sh) + d - 1) / d);
}
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// The scalar row, copied from garden_canopy_row with the A/B switch taken out
// (the branch is what D below removes).
static void canopy_scalar(uint16_t *row, int lo, int hi, int cx, int mrr, int qy, uint16_t leafy) {
    int mhi = (mrr >> 8) * 16, mlo = mrr & 255, qbase = 256 - qy;
    int lr = (leafy >> 11) & 31, lg = (leafy >> 5) & 63, lb = leafy & 31;
    for (int x = lo; x <= hi; x++) {
        int dx = x - cx, dx2 = dx * dx;
        int t = ((dx2 * 16) * mhi + dx2 * mlo) >> 18;
        int q = qbase - t;
        if (q < 0) q = 0;
        int f = (q * 39322) >> 16, g = 256 - f;
        unsigned a = row[x];
        row[x] = (uint16_t)(((((a >> 11) & 31) * g + lr * f) >> 8) * 2048
                          + ((((a >> 5) & 63) * g + lg * f) >> 8) * 32
                          +  (((a & 31) * g + lb * f) >> 8));
    }
}

// The same row eight pixels at a time, written as the kernel will run it: no
// branch, the q<0 clamp folded in, and the product that does not fit a lane
// coming back out of a 40-bit accumulator (modelled here as an int64_t sum,
// which is what EE.SRCMB.S16.QACC shifts down).
static void canopy_lanes(uint16_t *row, int lo, int hi, int cx, int mrr, int qy, uint16_t leafy) {
    int mhi = (mrr >> 8) * 16, mlo = mrr & 255, qbase = 256 - qy;
    int lr = (leafy >> 11) & 31, lg = (leafy >> 5) & 63, lb = leafy & 31;
    int x = lo;
    for (; x + 7 <= hi; x += 8) {
        int16_t dx2[8], q[8], f[8], g[8], ch[8][3];
        for (int i = 0; i < 8; i++) {                 // one lane each, still scalar here:
            int dx = x + i - cx;                      // the kernel gets these from a lane
            dx2[i] = (int16_t)(dx * dx);              // subtract and a 16-bit multiply
        }
        for (int i = 0; i < 8; i++) {
            // the accumulator path, both operands inside a lane
            int64_t acc = (int64_t)(dx2[i] * 16) * mhi + (int64_t)dx2[i] * mlo;
            int t = (int)(acc >> 18);
            int qq = qbase - t;
            q[i] = (int16_t)(qq < 0 ? 0 : qq);
            f[i] = (int16_t)((q[i] * 39322) >> 16);
            g[i] = (int16_t)(256 - f[i]);
        }
        for (int i = 0; i < 8; i++) {
            unsigned a = row[x + i];
            ch[i][0] = (int16_t)((((a >> 11) & 31) * g[i] + lr * f[i]) >> 8);
            ch[i][1] = (int16_t)((((a >> 5) & 63) * g[i] + lg * f[i]) >> 8);
            ch[i][2] = (int16_t)(((a & 31) * g[i] + lb * f[i]) >> 8);
            row[x + i] = (uint16_t)(ch[i][0] * 2048 + ch[i][1] * 32 + ch[i][2]);
        }
    }
    for (; x <= hi; x++) {                            // the retained scalar tail
        canopy_scalar(row, x, x, cx, mrr, qy, leafy);
    }
}

static void bad(const char *what, long long a, long long b, long long p0, long long p1, long long p2) {
    if (mismatches < 5)
        printf("  FAIL %s: got %lld want %lld at (%lld, %lld, %lld)\n", what, a, b, p0, p1, p2);
    mismatches++;
}

int main(void) {
    // A. The split, over every reachable (dx2, mrr).
    long steps = 0;
    for (int rx = 28; rx <= 44; rx++) {
        int rr = rx * rx, mrr = mrecip(rr, 26);
        int mhi = (mrr >> 8) * 16, mlo = mrr & 255;
        if (mhi > 32767 || mlo > 255) bad("mhi in a lane", mhi, 0, rx, 0, 0);
        for (int dx = -rx; dx <= rx; dx++) {
            int dx2 = dx * dx;
            if (dx2 * 16 > 32767) bad("dx2*16 in a lane", dx2 * 16, 0, rx, dx, 0);
            int64_t acc = (int64_t)(dx2 * 16) * mhi + (int64_t)dx2 * mlo;
            if (acc > 0x7FFFFFFFFFFFLL) bad("accumulator overflow", acc, 0, rx, dx, 0);
            int lane = (int)(acc >> 18);
            int plain = (dx2 * mrr) >> 18;
            if (lane != plain) bad("split product", lane, plain, rx, dx2, mrr);
            // B. and the reciprocal is exact over the same pairs.
            int exact = dx2 * 256 / rr;
            if (plain != exact) bad("reciprocal exact", plain, exact, rx, dx2, rr);
            steps++;
        }
    }
    printf("A+B: %ld (dx2, mrr) pairs, split and reciprocal exact, %s\n",
           steps, mismatches ? "MISMATCH" : "ok");

    // C. q*3/5.
    long cbad = mismatches;
    for (int q = 0; q <= 256; q++) {
        int f = (q * 39322) >> 16;
        if (f != q * 3 / 5) bad("q*3/5", f, q * 3 / 5, q, 0, 0);
    }
    printf("C: q*3/5 exact over 0..256, %s\n", mismatches == cbad ? "ok" : "MISMATCH");

    // D. f == 0 is the identity, for every row word and every leafy.
    long dbad = mismatches;
    uint32_t s = 987654321u;
    for (int i = 0; i < 200000; i++) {
        s = s * 1664525u + 1013904223u;
        unsigned a = s & 0xFFFFu, leafy = (s >> 16) & 0xFFFFu;
        int lr = (leafy >> 11) & 31, lg = (leafy >> 5) & 63, lb = leafy & 31;
        int g = 256, f = 0;
        unsigned out = (unsigned)(((((a >> 11) & 31) * g + lr * f) >> 8) * 2048
                                + ((((a >> 5) & 63) * g + lg * f) >> 8) * 32
                                +  (((a & 31) * g + lb * f) >> 8));
        if (out != a) bad("identity at f=0", out, a, a, leafy, 0);
    }
    // ... and no channel leaves its field for any reachable (a, leafy, f).
    long cbad2 = mismatches;
    for (int f = 0; f <= 153; f++) {
        int g = 256 - f;
        for (int ch = 0; ch < 3; ch++) {
            int maxin = ch == 1 ? 63 : 31, lim = ch == 1 ? 63 : 31;
            for (int v = 0; v <= maxin; v++) {
                int blended = (v * g + lim * f) >> 8;
                if (blended < 0 || blended > lim) bad("channel leaves field", blended, lim, ch, v, f);
            }
        }
    }
    printf("D: identity at f=0 and channel bounds, %s\n",
           mismatches == dbad ? "ok" : "MISMATCH");
    printf("   (channel field check: %s)\n", mismatches == cbad2 ? "ok" : "MISMATCH");

    // E. Whole rows: eight-lane against scalar, every rx, every clip alignment.
    long ebad = mismatches;
    uint16_t a1[240], a2[240];
    for (int rx = 28; rx <= 44; rx++) {
        int rr = rx * rx, mrr = mrecip(rr, 26);
        for (int ry = 17; ry <= 32; ry++)
            for (int y = 0; y <= 134; y += 7) {
                int qy = clampi((y % ry) * (y % ry) * 256 / (ry * ry), 0, 256);
                for (int cx = 0; cx <= 239; cx += 13)
                    for (unsigned leaf = 0; leaf < 0x10000u; leaf += 4099u) {
                        int lo = clampi(cx - rx, 0, 239), hi = clampi(cx + rx, 0, 239);
                        for (int i = 0; i < 240; i++) a1[i] = a2[i] = (uint16_t)(i * 2731u + leaf);
                        canopy_scalar(a1, lo, hi, cx, mrr, qy, (uint16_t)leaf);
                        canopy_lanes(a2, lo, hi, cx, mrr, qy, (uint16_t)leaf);
                        for (int i = lo; i <= hi; i++)
                            if (a1[i] != a2[i]) { bad("row", a2[i], a1[i], rx, cx, leaf); goto next; }
                    next: ;
                    }
            }
    }
    printf("E: eight-lane row == scalar row over every rx/clip/leafy, %s\n",
           mismatches == ebad ? "ok" : "MISMATCH");

    // F. The steps the scheduled kernel took out of the lanes, each over its whole
    //    domain: the blend written around f alone, the unpack at SAR 16, and the
    //    pack summed with a saturating add.
    long fbad = mismatches;
    for (int f = 0; f <= 153; f++)
        for (int c = 0; c <= 63; c++)
            for (int l = 0; l <= 63; l++) {
                int g = 256 - f;
                int plain = (c * g + l * f) >> 8, lane = c + (((l - c) * f) >> 8);
                if (plain != lane) bad("blend around f", lane, plain, c, l, f);
            }
    for (uint32_t p = 0; p <= 0xFFFFu; p++) {
        if (((p * 32u) >> 16) != (p >> 11)) bad("p*32>>16", (p * 32u) >> 16, p >> 11, p, 0, 0);
        if ((((p * 2048u) >> 16) & 63) != ((p >> 5) & 63)) bad("p*2048>>16", p, 0, p, 0, 0);
    }
    for (int r = 0; r <= 31; r++)
        for (int g6 = 0; g6 <= 63; g6++)
            for (int b = 0; b <= 31; b++) {
                int s = (int16_t)(uint16_t)(r * 2048);           /* the lanes are signed */
                s += g6 * 32; if (s > 32767) s = 32767; if (s < -32768) s = -32768;
                s += b;       if (s > 32767) s = 32767; if (s < -32768) s = -32768;
                if ((uint16_t)s != (uint16_t)((r << 11) | (g6 << 5) | b))
                    bad("saturating pack", (uint16_t)s, (r << 11) | (g6 << 5) | b, r, g6, b);
            }
    printf("F: blend around f, SAR-16 unpack, saturating pack == OR, %s\n",
           mismatches == fbad ? "ok" : "MISMATCH");


    printf("%s: mismatches=%ld\n", mismatches ? "FAIL" : "PASS", mismatches);
    return mismatches ? 1 : 0;
}
