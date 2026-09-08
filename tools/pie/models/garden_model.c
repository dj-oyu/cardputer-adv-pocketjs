// Exhaustive proof of the identities the garden pixel-loop kernel rests on.
//
// The kernel replaces every per-pixel integer division in garden_row's main
// loop, because PIE has no divide. Each replacement is a magic multiply, and
// each one is swept here over the whole domain the scene can actually produce
// rather than over the divisors somebody happened to try. That distinction is
// not academic: the first version of this design used ceil(2^24/ww) with a
// shift of 24, which computes u*u/ww and silently drops the factor 256 in
// u*u*256/ww. It inverted the sunlight lobe at its own edge -- q read 255 where
// the truth was 0 -- and no divisor value would have revealed it, only the
// range sweep below.
//
// Ranges come from garden_row itself, for y in 0..134:
//   width = 58 + y/3            -> 58..102,  ww  = width*width -> 3364..10404
//   w0    = 10 + y/13           -> 10..20,   ww0 = w0*w0       ->  100..400
//   w1    = 10 + y/8            -> 10..26,   ww1 = w1*w1       ->  100..676
// so the smallest divisor anywhere is 100. That is what fixes the exponent:
// ceil(2^24/100) = 167773 does not fit a 16-bit lane and ceil(2^22/100) = 41943
// does, so 2^22 with a shift of 14 is the only choice that serves every row.
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

static long mismatches;

// Round up, so that clamping an input to its own limit lands on exactly zero
// rather than one short of it. That is what lets the kernel drop the
// out-of-range compare and mask: u is clamped to +-width, and q must then be 0.
static int mrecip(int d, int sh) { return (int)((((int64_t)1 << sh) + d - 1) / d); }

// q = 256 - n*256/d, as the kernel computes it: one unsigned lane multiply for
// the square, one QACC pass for the magic, one VMAX against zero.
static int lobe_pie(int n, int d, int M) {
    (void)d;                    /* kept in the signature so the call sites read as a pair */
    int t = (int)(((uint64_t)(unsigned)n * (unsigned)M) >> 14);
    int q = 256 - t;
    return q < 0 ? 0 : q;
}
static int lobe_ref(int n, int d) {
    int q = 256 - n * 256 / d;
    return q < 0 ? 0 : q;
}

static void sweep_lobe(const char *name, int dlo, int dhi, int steplo, int stephi) {
    int worst = 0; long over = 0;
    for (int s = steplo; s <= stephi; s++) {
        int d = s * s;
        if (d < dlo || d > dhi) continue;
        int M = mrecip(d, 22);
        if (M > 65535) { printf("%s: magic %d does not fit a lane for d=%d\n", name, M, d); mismatches++; return; }
        // n is a square of a clamped offset, so it runs 0..d and no further.
        for (int u = 0; u <= s; u++) {
            int n = u * u;
            if ((uint64_t)(unsigned)n * (unsigned)M >> 14 > 65535) { printf("%s: intermediate overflows a lane\n", name); mismatches++; return; }
            int a = lobe_pie(n, d, M), b = lobe_ref(n, d);
            int e = abs(a - b);
            if (e > worst) worst = e;
            if (a > b) over++;
            // The property the kernel depends on for correctness rather than
            // for accuracy: at the clamp, the lobe must be exactly zero, or
            // out-of-band columns light up.
            if (u == s && a != 0) { printf("%s: d=%d clamped edge gives %d, not 0\n", name, d, a); mismatches++; }
        }
    }
    printf("%-10s divisors %5d..%-5d  max |error| %d  never above reference: %s\n",
           name, dlo, dhi, worst, over ? "NO" : "yes");
    if (worst > 1) { printf("%s: error exceeds one step\n", name); mismatches++; }
}

// The constant divisions in the same loop: density/12, density/20, sun/3.
static void sweep_const(const char *name, int d, int nhi) {
    int M = mrecip(d, 16), worst = 0;
    if (M > 65535) { printf("%s: magic does not fit a lane\n", name); mismatches++; return; }
    for (int n = 0; n <= nhi; n++) {
        int a = (int)(((uint64_t)(unsigned)n * (unsigned)M) >> 16), b = n / d;
        int e = abs(a - b);
        if (e > worst) worst = e;
        if (a < b) { printf("%s: n=%d gives %d, below %d\n", name, n, a, b); mismatches++; }
    }
    printf("%-10s /%-3d over 0..%-5d  magic %5d  max |error| %d\n", name, d, nhi, M, worst);
    if (worst > 0) { printf("%s: rounding up did not stay exact\n", name); mismatches++; }
}

// garden_smooth in lanes: t*t fits an unsigned lane at 65025 and the cubic is
// one QACC pass. This one is claimed bit-exact, so it is checked as such.
static void sweep_smooth(void) {
    int worst = 0;
    for (int t = 0; t < 256; t++) {
        int t2 = (t * t) & 0xffff, w = 768 - 2 * t;
        int a = (int)(((uint64_t)(unsigned)t2 * (unsigned)w) >> 16);
        int b = t * t * (768 - 2 * t) / 65536;
        if (a != b) { printf("smooth: t=%d gives %d, not %d\n", t, a, b); mismatches++; }
        if (a > worst) worst = a;
    }
    printf("%-10s exact over t=0..255, peak %d (fits a lane)\n", "smooth", worst);
}

// The lattice interpolation, rewritten so both terms are non-negative and one
// QACC pass carries them: a + (b-a)*f/256 becomes (a*(256-f) + b*f) >> 8. The
// two differ only where C truncates a negative product toward zero and the
// shift floors it, which is at most one step and only when b < a.
static void sweep_lerp(void) {
    int worst = 0; long moved = 0, total = 0;
    for (int a = 0; a < 256; a++)
        for (int b = 0; b < 256; b++)
            for (int f = 0; f < 256; f++) {
                int p = (int)(((uint64_t)(unsigned)a * (unsigned)(256 - f)
                             + (uint64_t)(unsigned)b * (unsigned)f) >> 8);
                int r = a + (b - a) * f / 256;
                int e = abs(p - r);
                if (e > worst) worst = e;
                if (e) moved++;
                total++;
                if (p < 0 || p > 255) { printf("lerp: %d out of range\n", p); mismatches++; }
            }
    printf("%-10s max |error| %d over %ld triples, %ld differ (%.2f%%), all in 0..255\n",
           "lerp", worst, total, moved, 100.0 * moved / total);
    if (worst > 1) { printf("lerp: error exceeds one step\n"); mismatches++; }
}

// sun = q*q*k >> 16 is regrouped as (q*k)*q >> 16 so that no intermediate
// leaves a 16-bit lane. q <= 256 and k <= 34, so q*k <= 8704. This is an
// identity, not an approximation, and is checked as one.
static void sweep_sun(void) {
    for (int q = 0; q <= 256; q++)
        for (int k = 12; k <= 34; k++) {
            int qk = (q * k) & 0xffff;
            int a = (int)(((uint64_t)(unsigned)qk * (unsigned)q) >> 16);
            int b = q * q * k / 65536;
            if (a != b) { printf("sun: q=%d k=%d gives %d, not %d\n", q, k, a, b); mismatches++; }
            if (q * k > 65535) { printf("sun: q*k=%d leaves a lane\n", q * k); mismatches++; }
        }
    puts("sun        (q*k)*q>>16 == q*q*k>>16 exactly, for q 0..256 and k 12..34");
}


// The shoulder, regrouped. s*s leaves a lane at s=256 and s*s*gain*haze leaves
// even QACC's 40 bits nowhere useful, so it is taken in two passes: (s*gain)*s
// shifted by 8, then *haze shifted by 16. Two floors instead of one, so this is
// a bound rather than an identity and is swept as such over every haze the row
// can produce (4*haze = 320 + S, S 0..1020) and both gains. The kernel
// carries four times the haze because the fine octave leaves the density
// undivided; the extra quarter is taken in this shift.
static void sweep_shoulder(void) {
    int worst = 0; long over = 0, total = 0;
    for (int gain = 4; gain <= 7; gain += 3)
        for (int haze = 320; haze <= 1340; haze++)
            for (int s = 0; s <= 256; s++) {
                int sg = (s * gain) & 0xffff;
                if (s * gain > 65535) { printf("shoulder: s*gain leaves a lane\n"); mismatches++; return; }
                int t = (int)(((uint64_t)(unsigned)sg * (unsigned)s) >> 8);
                if (t > 65535) { printf("shoulder: intermediate %d leaves a lane\n", t); mismatches++; return; }
                int a = (int)(((uint64_t)(unsigned)t * (unsigned)haze) >> 18);
                int b = s * s * gain * (haze / 4) / (65536 * 256);
                int e = abs(a - b);
                if (e > worst) worst = e;
                if (a > b) over++;
                total++;
            }
    printf("%-10s two-pass floor vs one, gains 4 and 7, 4*haze 320..1340: max |error| %d over %ld (%ld high)\n",
           "shoulder", worst, total, over);
    if (worst > 1) { printf("shoulder: error exceeds one step\n"); mismatches++; }
}

// The channel shifts. ambient/2 and sun*5/2 floor separately in the scalar
// loop; one QACC pass floors their sum, which is one instruction instead of
// eight and moves the 8-bit channel by at most one before the 565 truncation
// throws three bits away. Green is the same trade at a shift of two. The point
// of sweeping it is the *packing*: cr and cg have to stay below 256 or the
// multiply-and-mask that places them aliases into the next field.
static void sweep_channels(void) {
    int wr = 0, wg = 0, hicr = 0, hicg = 0, hicb = 0;
    for (int amb = 0; amb <= 49; amb++)
        for (int sun = 0; sun <= 48; sun++) {
            int cr = ((amb + 5 * sun) >> 1) + 3 + 10;
            int cg = ((3 * amb + 6 * sun) >> 2) + 3 + 22;
            int cb = (amb + (int)(((uint64_t)(unsigned)sun * 21846u) >> 16) + 3 + 28) >> 3;
            int rr = amb / 2 + sun * 5 / 2 + 3 + 10;
            int rg = amb * 3 / 4 + sun * 3 / 2 + 3 + 22;
            if (abs(cr - rr) > wr) wr = abs(cr - rr);
            if (abs(cg - rg) > wg) wg = abs(cg - rg);
            if (cr > hicr) hicr = cr;
            if (cg > hicg) hicg = cg;
            if (cb > hicb) hicb = cb;
            // The placement: (v>>3)<<11 is (v<<8) masked, (v>>2)<<5 is (v<<3)
            // masked. Only true while v stays under 256.
            if (((cr * 256) & 0xF800) != ((cr >> 3) << 11)) { printf("pack: red aliases at %d\n", cr); mismatches++; }
            if (((cg * 8) & 0x07E0) != ((cg >> 2) << 5)) { printf("pack: green aliases at %d\n", cg); mismatches++; }
        }
    printf("%-10s combined shift costs r %d g %d; peaks cr=%d cg=%d cb=%d (all fit their field)\n",
           "channels", wr, wg, hicr, hicg, hicb);
    if (wr > 1 || wg > 1) { printf("channels: combined shift moved more than one step\n"); mismatches++; }
    if (hicr > 255 || hicg > 255 || hicb > 31) { printf("channels: a field overflows\n"); mismatches++; }
}

// The dither, as lanes. Its statistics are tools/test_garden.c's business; what
// belongs here is that the arithmetic fits: h*h and s*KM are full 32-bit
// products taken out of QACC's 40 bits, and the row term is XORed rather than
// added because EE.VADDS.S16 saturates.
static void sweep_dither(void) {
    unsigned bin[4] = {0, 0, 0, 0};
    for (unsigned h = 0; h < 65536; h++) {
        uint64_t sq = (uint64_t)h * h;
        if (sq >= (1ull << 40)) { printf("dither: h*h leaves QACC\n"); mismatches++; return; }
        unsigned s = (unsigned)((sq >> 8) & 0xffffu);
        uint64_t p = (uint64_t)s * 3761u;
        if (p >= (1ull << 40)) { printf("dither: s*KM leaves QACC\n"); mismatches++; return; }
        bin[(unsigned)((p >> 8) & 3u)]++;
    }
    printf("%-10s over all 65536 lane values: %u/%u/%u/%u\n", "dither", bin[0], bin[1], bin[2], bin[3]);
}

// The canopy's per-pixel divide, replaced exactly rather than approximately.
// rx runs 28..44 so rr = rx*rx runs 784..1936, and the ellipse is clipped so
// that |dx| <= rx: the numerator never exceeds the divisor. Over that domain a
// round-up reciprocal at 2^26 with a shift of 18 is not a bound, it is an
// identity -- which is why the canopy change moves no pixels at all, unlike the
// sunlight lobe's reciprocals above. The shift is the smallest that is exact
// (16 and 17 are out by one) and the largest that keeps dx*dx*M inside a signed
// 32-bit int, which is a two-sided constraint worth stating: there is no slack.
// int64_t and not long: `long` is 32 bits on Windows, where this model is
// usually built, and 64 on the WSL side. The widest term here is 67 million
// and the overflow guard compares against 2^40, which on a 32-bit long is a
// shift past the width of the type -- the guard would have been the thing
// that overflowed. Same trap as tools/test_solar_time.c and CLAUDE.md.
static void sweep_canopy(void) {
    int64_t checked = 0, maxprod = 0;
    for (int rx = 28; rx <= 44; rx++) {
        int d = rx * rx;
        int64_t M = (((int64_t)1 << 26) + d - 1) / d;
        for (int u = 0; u <= rx; u++) {
            int64_t n = (int64_t)u * u, prod = n * M;
            if (prod > maxprod) maxprod = prod;
            if (prod > INT32_MAX) { printf("canopy: dx*dx*M leaves a signed int\n"); mismatches++; return; }
            if ((prod >> 18) != n * 256 / d) {
                printf("canopy: rr=%d dx2=%lld gives %lld, not %lld\n", d, (long long)n, (long long)(prod >> 18), (long long)(n * 256 / d));
                mismatches++;
            }
            checked++;
        }
    }
    printf("%-10s exact over %lld reachable pairs, rr 784..1936; widest product %lld of 2^31\n",
           "canopy", (long long)checked, (long long)maxprod);
}

// The canopy span, as garden_canopy_row now computes it. Three claims, and all
// three have to be exact rather than bounded, because this replaced working
// scalar code that was already exact -- an approximation here would move the
// picture for nothing.
//
// 1. mrr = ceil(2^26/rr) reaches 85,598 and does not fit a 16-bit lane, so a
//    vector form has to split it. hi*256+lo, with dx2*hi*256 taken as
//    (dx2*16)*(hi*16): dx2 <= 1936 so dx2*16 fits, hi <= 334 so hi*16 fits.
// 2. q*3/5 as one multiply and a shift.
// 3. clamping q at zero replaces the `if(q>0)`, because blending with alpha
//    zero is the identity: (A*256 + B*0) >> 8 is A.
static void sweep_canopy_span(void) {
    int64_t worst_t = 0, widest = 0;
    for (int rx = 28; rx <= 44; rx++) {
        int rr = rx * rx;
        int64_t mrr = (((int64_t)1 << 26) + rr - 1) / rr, hi = mrr >> 8, lo = mrr & 255;
        if (hi * 16 > 65535) { printf("canopy span: hi*16 leaves a lane\n"); mismatches++; return; }
        for (int u = 0; u <= rx; u++) {
            int64_t dx2 = (int64_t)u * u;
            if (dx2 * 16 > 65535) { printf("canopy span: dx2*16 leaves a lane\n"); mismatches++; return; }
            int64_t acc = (dx2 * 16) * (hi * 16) + dx2 * lo;
            if (acc > widest) widest = acc;
            if (acc >= ((int64_t)1 << 40)) { printf("canopy span: QACC overflows\n"); mismatches++; return; }
            if ((acc >> 18) != ((dx2 * mrr) >> 18)) {
                printf("canopy span: rr=%d dx2=%lld split gives %lld, not %lld\n",
                       rr, (long long)dx2, (long long)(acc >> 18), (long long)((dx2 * mrr) >> 18));
                mismatches++;
            }
            if ((acc >> 18) > worst_t) worst_t = acc >> 18;
        }
    }
    int64_t M = (65536L * 3 + 4) / 5;
    if (M > 65535) { printf("canopy span: the alpha magic does not fit a lane\n"); mismatches++; }
    for (int q = 0; q <= 256; q++)
        if ((((int64_t)q * M) >> 16) != (int64_t)q * 3 / 5) {
            printf("canopy span: q=%d alpha gives %lld, not %d\n", q, (long long)(((int64_t)q * M) >> 16), q * 3 / 5);
            mismatches++;
        }
    for (int A = 0; A < 64; A++)
        if (((int64_t)A * 256 + 0) >> 8 != A) { printf("canopy span: zero alpha is not the identity\n"); mismatches++; }
    printf("%-10s split exact (peak t=%lld, widest QACC term %lld of 2^40); q*3/5 == (q*%lld)>>16;"
           " zero alpha is the identity\n", "canopy2", (long long)worst_t, (long long)widest, (long long)M);
}

int main(void) {
    sweep_smooth();
    sweep_lerp();
    sweep_sun();
    sweep_lobe("lobe ww", 3364, 10404, 58, 102);
    sweep_lobe("lobe ww0", 100, 400, 10, 20);
    sweep_lobe("lobe ww1", 100, 676, 10, 26);
    // Taken on the undivided sum S = 3Dc+Df (0..1020): floor(S/48) is
    // floor((S/4)/12) and floor(S/80) is floor((S/4)/20), which is what lets
    // the fine octave skip its shift entirely.
    sweep_const("density", 48, 1020);
    sweep_const("density", 80, 1020);
    sweep_const("sun", 3, 63);
    sweep_shoulder();
    sweep_channels();
    sweep_dither();
    sweep_canopy();
    sweep_canopy_span();
    printf("mismatches=%ld\n", mismatches);
    return mismatches ? 1 : 0;
}
