// Host test of main/pocket/proc_band_hash.h against the murmur3 entry hash it
// replaced (docs/vm/qjs-observe.md sec.22): avalanche (every input bit
// flips every output bit with p ~ 0.5) and collisions on structured inputs
// (coordinates and colours that differ a little, as frames do), compared
// with what a random 32-bit function gives.
//   gcc -std=gnu11 -O2 -Wall -Wextra -Werror -Imain -Imain/pocket tools/test_band_hash.c -o /tmp/t && /tmp/t
#include "proc_band_hash.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rotl32(uint32_t v, unsigned r) { return v << r | v >> (32u - r); }
static uint32_t murmur_entry(const ksn_proc_segment *s, unsigned n) {   // the old one, verbatim
    uint32_t h = 0x2545f491u;
    for (unsigned k = 0; k < n; k++) {
        const uint32_t w[3] = {(uint16_t)s[k].x0 | (uint32_t)(uint16_t)s[k].y0 << 16,
                               (uint16_t)s[k].x1 | (uint32_t)(uint16_t)s[k].y1 << 16, s[k].color};
        for (unsigned j = 0; j < 3; j++) {
            uint32_t v = w[j] * 0xcc9e2d51u;
            v = rotl32(v, 15) * 0x1b873593u;
            h = rotl32(h ^ v, 13) * 5u + 0xe6546b64u;
        }
    }
    h ^= n; h ^= h >> 16; h *= 0x85ebca6bu; h ^= h >> 13; h *= 0xc2b2ae35u; h ^= h >> 16;
    return h;
}
typedef uint32_t (*hash_fn)(const ksn_proc_segment *, unsigned);
static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)rng; }
static int16_t coord(void) { return (int16_t)((int)(rnd() % 1201) - 480); }
static void random_seg(ksn_proc_segment *s) {
    s->x0 = coord(); s->y0 = coord(); s->x1 = coord(); s->y1 = coord(); s->color = (uint16_t)rnd();
}

// worst |p - 0.5| over (input bit, output bit), n segments an entry
static double avalanche(hash_fn f, unsigned n, unsigned trials) {
    static unsigned flips[160][32];
    memset(flips, 0, sizeof flips);
    for (unsigned t = 0; t < trials; t++) {
        ksn_proc_segment s[2], m[2];
        random_seg(&s[0]); random_seg(&s[1]);
        const uint32_t h = f(s, n);
        for (unsigned bit = 0; bit < 80u * n; bit++) {
            memcpy(m, s, sizeof s);
            ((uint8_t *)&m[bit / 80])[(bit % 80) / 8] ^= (uint8_t)(1u << (bit % 8));
            const uint32_t d = h ^ f(m, n);
            for (unsigned o = 0; o < 32; o++) flips[bit][o] += d >> o & 1u;
        }
    }
    double worst = 0;
    for (unsigned bit = 0; bit < 80u * n; bit++)
        for (unsigned o = 0; o < 32; o++) {
            const double p = (double)flips[bit][o] / trials, e = fabs(p - 0.5);
            if (e > worst) worst = e;
        }
    return worst;
}

static int cmp32(const void *a, const void *b) {
    const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}
// collisions among distinct structured entries
static unsigned long collisions(hash_fn f, int kind, unsigned *count) {
    static uint32_t h[1u << 21];
    unsigned k = 0;
    for (int a = 0; a < 64; a++)
        for (int b = 0; b < 64; b++)
            for (int c = 0; c < 16; c++)
                for (int d = 0; d < 16 && k < (1u << 21); d++) {
                    ksn_proc_segment s = {0};
                    if (kind == 0) {        // a line moving a little: x0,y0 and its delta
                        s.x0 = (int16_t)a; s.y0 = (int16_t)b; s.x1 = (int16_t)(a + c); s.y1 = (int16_t)(b + d);
                        s.color = 0xF800;
                    } else {                // fixed geometry, a palette of colours, and position
                        s.x0 = (int16_t)(a * 3 - 100); s.y0 = (int16_t)(b * 2); s.x1 = (int16_t)(s.x0 + 7);
                        s.y1 = s.y0; s.color = (uint16_t)(c * 4096 + d * 17);
                    }
                    h[k++] = f(&s, 1);
                }
    qsort(h, k, sizeof h[0], cmp32);
    unsigned long n = 0;
    for (unsigned i = 1; i < k; i++) n += h[i] == h[i - 1];
    *count = k;
    return n;
}

int main(void) {
    int fails = 0;
    const struct { const char *name; hash_fn f; } fs[] = {{"murmur3 (old)", murmur_entry},
                                                         {"mum (new)", proc_band_entry_hash}};
    for (unsigned i = 0; i < 2; i++) {
        const double a1 = avalanche(fs[i].f, 1, 20000), a2 = avalanche(fs[i].f, 2, 8000);
        unsigned n0, n1;
        const unsigned long c0 = collisions(fs[i].f, 0, &n0), c1 = collisions(fs[i].f, 1, &n1);
        const double e0 = (double)n0 * n0 / 8589934592.0, e1 = (double)n1 * n1 / 8589934592.0;
        printf("%-14s avalanche worst |p-0.5| 1 seg %.4f, 2 segs %.4f; collisions moving-line %lu of %u "
               "(random 32-bit: %.0f), palette %lu of %u (%.0f)\n",
               fs[i].name, a1, a2, c0, n0, e0, c1, n1, e1);
        if (i == 1) {
            // 20,000 trials put 3 sigma at ~0.011; a structural weakness shows as far more.
            if (a1 > 0.02 || a2 > 0.025) { printf("FAIL avalanche\n"); fails++; }
            if (c0 > 3 * e0 + 30 || c1 > 3 * e1 + 30) { printf("FAIL collisions\n"); fails++; }
        }
    }
    printf("BAND_HASH %s: %d failure(s)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
