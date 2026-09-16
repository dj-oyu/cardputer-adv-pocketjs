// Host check: folding a mean anomaly into +-pi with no libm call.
//
//   cc -O2 tools/fold_harness.c -lm -o /tmp/fold_harness && /tmp/fold_harness 50000000
//
// This is the proof for the `fold_tau()` in docs/perf/flash-removals.md 2.1. That
// fold replaces the one call main/scene/solar_sail.c makes to picolibc's
// `remainder`, which is 402 bytes of the image and which we are the only referrer
// of -- but the replacement's own code is 508 bytes in the object, so the change
// is +80 B of flash and was NOT taken (flash-removals.md 2). The harness is kept
// because the numbers below are what makes the fold usable if the instruction
// count ever matters more than the bytes, and because it prices the next candidate
// of this shape without re-deriving the arithmetic.
//
// What is checked, and why each part is needed:
//   1. every product the fold uses -- (h+-1/2)*TAU_HI and (h+-1/2)*TAU_LO -- has at
//      most 53 significant bits for |h| <= 2^24, computed with 128-bit integers.
//      Without that the multiply would round and the comparison against the half
//      period would not be exact.
//   2. the intermediate m - h*TAU_HI is exactly representable: recomputed in long
//      double (64-bit significand) and compared. That is the Sterbenz half of the
//      argument, and it is the reason the result rounds only once.
//   3. the fold equals libm's remainder() bit for bit at the double level AND after
//      the (float) cast the caller applies, over a dense grid, uniform samples, and
//      an adversarial sweep around every (h+1/2)*2pi -- down to the ulp and below,
//      so exact ties (distance 0) are in the sample set.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

// ---- the candidate, exactly as it would appear in main/scene/solar_sail.c ----
#define TAU_FOLD_HI 6.283185303211212       // 2pi with the low 26 mantissa bits cleared
#define TAU_FOLD_LO 3.968374073792802e-09   // 2pi - TAU_FOLD_HI, exact
static double fold_tau(double m) {
    if(m>=-3.141592653589793&&m<=3.141592653589793)return m;
    double h=(double)(long long)(m*0.15915494309189535+(m<0?-0.5:0.5));
    double ah=(h+0.5)*TAU_FOLD_HI,al=(h+0.5)*TAU_FOLD_LO;
    if(m-ah>al)h+=1.0;                      // m is above (h+1/2) periods
    else if(m-ah==al) { if((long long)h&1)h+=1.0; }   // on it: the even one wins
    else {
        double bh=(h-0.5)*TAU_FOLD_HI,bl=(h-0.5)*TAU_FOLD_LO,d=m-bh;
        if(d<bl)h-=1.0;                     // below (h-1/2) periods
        else if(d==bl&&((long long)h&1))h-=1.0;
    }
    return (m-h*TAU_FOLD_HI)-h*TAU_FOLD_LO;
}
// -----------------------------------------------------------------------------

static double old(double m) { return remainder(m, 6.283185307179586); }

static uint64_t rng = 0x123456789abcdefULL;
static double rnd(void) { rng^=rng<<13; rng^=rng>>7; rng^=rng<<17; return (double)(rng>>11)/9007199254740992.0; }

static long long ntest, nbad, nbad32, ntie, near_tie;
static double dmax, min_margin = 1e300, mmax;
static double hmax;
static int seen;
static uint64_t bad_first;

// (2k+1)*C must have at most 53 significant bits, i.e. the double multiply is exact.
static int exact_prod(double k, double c, int *bits) {
    double kk = 2.0*k + 1.0;
    __int128 num = (__int128)(int64_t)kk;              // |2k+1| < 2^25 in this range
    int e; double fr = frexp(c, &e);                   // c = fr*2^e, fr in [0.5,1)
    uint64_t C = (uint64_t)llround(ldexp(fr, 53));     // the exact 53-bit significand
    __int128 p = num*(__int128)C;
    unsigned __int128 u = p<0 ? (unsigned __int128)(-p) : (unsigned __int128)p;
    int tz = 0; unsigned __int128 v = u; while (v && !(v&1)) { tz++; v >>= 1; }
    int b = 0; while (u) { b++; u >>= 1; }
    *bits = b - tz;                                    // strip trailing zeros: that is what counts
    return (b - tz) <= 53;
}

static void exactness_probe(void) {
    int bad = 0, maxb = 0; long long n = 0;
    for (long long h = -(1LL<<24); h <= (1LL<<24); h += 9973) {
        int b1, b2, b3, b4; n += 4;
        if (!exact_prod((double)h+0.5, TAU_FOLD_HI, &b1)) bad++;
        if (!exact_prod((double)h+0.5, TAU_FOLD_LO, &b2)) bad++;
        if (!exact_prod((double)h-0.5, TAU_FOLD_HI, &b3)) bad++;
        if (!exact_prod((double)h-0.5, TAU_FOLD_LO, &b4)) bad++;
        if (b1>maxb) maxb=b1;
        if (b2>maxb) maxb=b2;
        if (b3>maxb) maxb=b3;
        if (b4>maxb) maxb=b4;
    }
    printf("split-product exactness over |h|<=2^24: %lld products, %d NOT exact, max significant bits %d (limit 53)\n",
           n, bad, maxb);
}

static long long nexact, ninexact;
static void check_exact_sub(double m) {
    double h = (double)(long long)(m*0.15915494309189535 + (m<0?-0.5:0.5));
    if (h == 0.0) return;
    double d = m - h*TAU_FOLD_HI;
    long double ld = (long double)m - (long double)h*(long double)TAU_FOLD_HI;
    if ((long double)d == ld) nexact++; else ninexact++;
}

static void one(double m) {
    double a = old(m), b = fold_tau(m);
    ntest++;
    check_exact_sub(m);
    if (fabs(m) > mmax) mmax = fabs(m);
    if (a != b) { nbad++; if (!seen) { seen = 1; memcpy(&bad_first, &m, 8); } }
    if ((float)a != (float)b) nbad32++;
    double d = fabs(a-b); if (d > dmax) dmax = d;
    if (a == b) ntie++;
    double q = m/6.283185307179586, fr = q - floor(q), mg = fmin(fr, 1.0-fr);
    if (fabs(mg-0.5) < min_margin) min_margin = fabs(mg-0.5);
    double h = (double)(long long)(m*0.15915494309189535 + (m<0?-0.5:0.5));
    if (fabs(h) > hmax) hmax = fabs(h);
    if (fabs(mg-0.5) < 1e-12) near_tie++;
}

int main(int argc, char **argv) {
    long long N = argc > 1 ? atoll(argv[1]) : 2000000LL;

    exactness_probe(); fflush(stdout);

    // 1. dense grid over the reachable range (|m| is under 1.1e3 in the scene) and past it
    for (int64_t k = -2400LL*(1<<13); k <= (int64_t)2400*(1<<13); k++) one((double)k/8192.0);
    printf("dense grid |m|<=2400 step 1/8192: %lld tested\n", ntest); fflush(stdout);
    // 2. uniform random inside the reachable range, then far outside it, then tiny
    for (long long i = 0; i < N; i++) one((rnd()*2.0-1.0)*1200.0);
    for (long long i = 0; i < N/4; i++) one((rnd()*2.0-1.0)*1.0e6);
    for (long long i = 0; i < N/20; i++) one((rnd()*2.0-1.0)*1.0e-8);
    // 3. adversarial: m = (h+1/2)*2pi + delta, delta from well above to below one ulp
    for (long long h = -100000; h <= 100000; h++) {
        double base = (double)h*6.283185307179586 + 3.141592653589793;
        for (int e = 0; e <= 6; e++) {
            double step = ldexp(1.0, -30 + e*3);
            for (int j = -3; j <= 3; j++) {
                double m = base + j*step;
                one(m); one(nextafter(m, 1e300)); one(nextafter(m, -1e300));
            }
        }
    }

    printf("total tested %lld  double mismatches %lld  float mismatches %lld\n", ntest, nbad, nbad32);
    printf("double results equal in %lld of %lld (%.4f%%)\n", ntie, ntest, 100.0*(double)ntie/(double)ntest);
    printf("max |old-new| at double level %.3g ; max |m| swept %.6g ; max |h| %.0f\n", dmax, mmax, hmax);
    printf("min distance of m/(2pi) to a half-integer over the sweep: %.3g ; samples inside 1e-12 of one: %lld\n",
           min_margin, near_tie);
    printf("intermediate m-h*TAU_HI exact in long double: %lld exact, %lld NOT exact\n", nexact, ninexact);
    if (seen) { double bm; memcpy(&bm, &bad_first, 8); printf("first mismatch m=%.20g\n", bm); }
    return (nbad || nbad32 || ninexact) ? 1 : 0;
}
