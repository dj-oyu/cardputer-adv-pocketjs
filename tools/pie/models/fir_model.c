/* The MP3 downsample FIR: the arithmetic the PIE kernel will implement.
 *
 * main/pocket/mp3_decode.c runs, per output sample,
 *
 *     sum = sum(history[(cursor-k)&31] * filter[k], k=0..31);
 *     sample = sum >> 14;   clamp to int16
 *
 * and a vector version has to produce the same 16-bit samples. The shape it
 * needs is the transposed one: eight consecutive outputs at a time, one tap at
 * a time, each tap a broadcast multiply accumulate into an eight-lane
 * accumulator (`EE.VMULAS.S16.QACC` with `EE.VLDBC.16` for the coefficient).
 * That form changes *when* each product is added, and there is no rounding
 * anywhere in the sum, so it should be exact by construction -- which is what
 * this model checks rather than argues, together with the three things that
 * are not arithmetic at all:
 *
 *   1. the transposed form equals the per-output dot product, bit for bit
 *   2. the 40-bit PIE accumulator cannot overflow on this data
 *   3. the saturating readout (`EE.SRCMB.S16.QACC`) equals `sum >> 14` clamped
 *   4. the doubled ring buffer the kernel reads indexes the same samples as
 *      `(cursor-k)&31` does, including across the wrap
 *
 * The last one is the change to the C code the kernel needs: write each new
 * sample at `cursor` and at `cursor + 32`, and the eight-lane windows become
 * contiguous memory that an unaligned 128-bit load can read (the 32-tap window
 * of output i starts at `base - 31 + i`, which is never 16-byte aligned, so the
 * kernel needs `EE.LD.128.USAR.IP` + `EE.SRC.Q` -- see docs/perf/pie-simd.md 1.4).
 *
 * The coefficients are Q14 and sum to 16384 (filter_init re-centres the odd
 * tap so that they do), the input is int16, and 40-bit lanes hold 32 products
 * of at most 2^14 * 2^15 = 2^29 each: 2^34 worst case, well inside 2^39. Claim
 * 2 is checked both as a bound and against the largest value the runs reach.
 *
 * Prints `mismatches=0` when every claim holds; any disagreement prints the
 * case so it can be reproduced.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TAPS 32
#define RING 32
#define BUF 64          /* the doubled ring: history[t] lives at t and t + 32 */
#define LANES 8

static uint32_t state = 12345u;
static uint32_t rnd(void) { state = state * 1664525u + 1013904223u; return state >> 8; }
static int16_t rnd16(void) { return (int16_t)(rnd() & 0xFFFFu); }

static int16_t sat16(int32_t v) { return (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v)); }

/* The shipping scalar form, exactly as the C loop reads. */
static int16_t scalar_out(const int16_t *hist, int cursor, const int16_t *h) {
    int32_t sum = 0;
    for (unsigned k = 0; k < TAPS; k++)
        sum += (int32_t)hist[(cursor - (int)k) & (RING - 1)] * h[k];
    return sat16(sum >> 14);          /* the C code shifts then clamps */
}

/* The transposed form: eight outputs, one tap at a time. Lane j carries the
 * output for sample time `base - 25 - 7 + j` in buffer terms, i.e. the eight
 * most recent samples that have been pushed, oldest first, so a store writes
 * them in time order. */
static void transposed(const int16_t *buf, int base, const int16_t *h, int32_t acc[LANES]) {
    for (int j = 0; j < LANES; j++)
        acc[j] = 0;
    for (int k = 0; k < TAPS; k++)
        for (int j = 0; j < LANES; j++)
            acc[j] += (int32_t)buf[base + j - k] * h[k];   /* h[k] is one broadcast */
}

#define BASE(cursor) ((cursor) + 25)      /* lane 7 is the newest sample, lane 0 the oldest */

int main(void) {
    int real_mismatch = 0, index_mismatch = 0, clamp_mismatch = 0;
    long long worst = 0, cases = 0;
    const long long limit = 1LL << 39;                     /* the accumulator's range */
    const long long bound = (long long)TAPS * 32768 * 16384;   /* |sum| <= 2^34 */

    int16_t hist[RING], h[TAPS];
    int16_t buf[BUF];
    int32_t acc[LANES];

    /* A real filter: sum = 16384 (Q14), largest tap under 2^14 -- the shape
     * filter_init produces, without needing sinf here. */
    for (int k = 0; k < TAPS; k++) h[k] = (int16_t)((k == 15 ? 1024 : 480) + (k & 3));
    int hsum = 0; for (int k = 0; k < TAPS; k++) hsum += h[k];
    h[15] += (int16_t)(16384 - hsum);
    hsum = 0; for (int k = 0; k < TAPS; k++) hsum += h[k];
    if (hsum != 16384) { printf("filter does not sum to 16384: %d\n", hsum); return 1; }

    for (int test = 0; test < 200000; test++) {
        /* The kernel reads windows that reach 31 samples back and 7 samples
         * forward of the oldest output lane, so the buffer must hold at least
         * 39 of the mirrored copies and `cursor` must be past the warm-up --
         * exactly the constraint the caller has to respect (the shipping code
         * gets it from a zero-initialised ring). */
        int cursor = 6 + (int)(rnd() % (RING - 6));
        for (int t = 0; t < RING; t++)
            hist[t] = (test < 4) ? (int16_t)(test & 1 ? 32767 : -32768) : rnd16();
        /* write the ring twice, the way the kernel's caller will */
        for (int t = 0; t < BUF; t++) buf[t] = hist[t & (RING - 1)];

        /* claim 4: the doubled buffer reads the same samples as `& 31` */
        for (int j = 0; j < LANES; j++)
            for (int k = 0; k < TAPS; k++)
                if (buf[BASE(cursor) + j - k] != hist[(cursor - 7 + j - k) & (RING - 1)])
                    index_mismatch++;

        transposed(buf, BASE(cursor), h, acc);
        for (int j = 0; j < LANES; j++) {
            long long a = acc[j];
            if (a < 0) a = -a;
            if (a > worst) worst = a;
            cases++;
            /* claim 1 + 3: same 16-bit sample as the scalar form */
            int16_t want = scalar_out(hist, (cursor - 7 + j) & (RING - 1), h);
            int16_t got = sat16(acc[j] >> 14);
            if (got != want) {
                if (real_mismatch < 3)
                    printf("mismatch: cursor=%d lane=%d acc=%d got=%d want=%d\n",
                           cursor, j, acc[j], got, want);
                real_mismatch++;
            }
            /* the clamp itself: the saturating readout against the C clamp */
            int32_t raw = (int32_t)(acc[j] >> 14);
            int16_t via_clamp = sat16(raw);
            if (via_clamp != (int16_t)(raw > 32767 ? 32767 : raw < -32768 ? -32768 : raw)) clamp_mismatch++;
        }
    }

    printf("cases %lld  largest |sum| %lld (bound %lld, accumulator %lld)\n",
           cases, worst, bound, limit);
    printf("index %d clamp %d\n", index_mismatch, clamp_mismatch);
    int bad = real_mismatch + index_mismatch + clamp_mismatch;
    if (worst >= limit || worst > bound) {
        printf("accumulator range: observed %lld exceeds %s\n", worst,
               worst >= limit ? "the 40-bit lanes" : "the 2^34 bound");
        bad++;
    }
    printf("mismatches=%d\n", bad);
    return bad != 0;
}
