#include "ksn_proc_points_pie.h"

#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct __attribute__((aligned(16))) {
    int16_t x[40], y[40], dx[40], dy[40];
} Planes;

static uint32_t rng = UINT32_C(0x7a31c195);
static uint32_t next_random(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static int16_t oracle(int64_t sum)
{
    int64_t v = sum / INT64_C(16384);
    if (sum < 0 && sum % INT64_C(16384)) --v;
    if (v > INT16_MAX) return INT16_MAX;
    if (v < INT16_MIN) return INT16_MIN;
    return (int16_t)v;
}

static void check_case(size_t n, KsnProcAffineQ14 m, int alias)
{
    Planes p;
    memset(&p, 0x5a, sizeof p);
    int16_t ex[40], ey[40], original_x[40], original_y[40];
    for (size_t i = 0; i < 40; ++i) {
        p.x[i] = (int16_t)next_random();
        p.y[i] = (int16_t)next_random();
    }
    if (n > 0) {
        p.x[0] = INT16_MIN; p.y[0] = INT16_MIN;
    }
    if (n > 1) {
        p.x[1] = INT16_MAX; p.y[1] = INT16_MAX;
    }
    memcpy(ex, p.x, sizeof ex);
    memcpy(ey, p.y, sizeof ey);
    memcpy(original_x, p.x, sizeof original_x);
    memcpy(original_y, p.y, sizeof original_y);
    for (size_t i = 0; i < n; ++i) {
        const int64_t x = p.x[i], y = p.y[i];
        ex[i] = oracle((int64_t)m.m00*x + (int64_t)m.m01*y + m.tx);
        ey[i] = oracle((int64_t)m.m10*x + (int64_t)m.m11*y + m.ty);
    }
    KsnProcPointDst d = { (alias & 1) ? p.x : p.dx,
                          (alias & 2) ? p.y : p.dy };
    ksn_proc_points_affine_pie(d, (KsnProcPointSrc){p.x, p.y}, n, &m);
    for (size_t i = 0; i < n; ++i) {
        assert(d.x[i] == ex[i]);
        assert(d.y[i] == ey[i]);
    }
    for (size_t i = n; i < 40; ++i) {
        assert(d.x[i] == ((alias & 1) ? ex[i] : (int16_t)0x5a5a));
        assert(d.y[i] == ((alias & 2) ? ey[i] : (int16_t)0x5a5a));
    }
    if (!(alias & 1)) assert(memcmp(p.x, original_x, sizeof p.x) == 0);
    if (!(alias & 2)) assert(memcmp(p.y, original_y, sizeof p.y) == 0);
}

static void extraction_boundaries(void)
{
    _Alignas(16) int16_t x[8] = {-32768, -32767, -1, 0, 1, 2, 32766, 32767};
    _Alignas(16) int16_t y[8] = {0};
    _Alignas(16) int16_t dx[8], dy[8];
    const int32_t offsets[] = {-16385, -16384, -16383, -1, 0, 1,
                               16383, 16384, 16385};
    for (unsigned i = 0; i < sizeof offsets / sizeof offsets[0]; ++i) {
        KsnProcAffineQ14 m = {16384, 0, -16384, 0, offsets[i], offsets[i]};
        ksn_proc_points_affine_pie((KsnProcPointDst){dx, dy},
                                   (KsnProcPointSrc){x, y}, 8, &m);
        for (unsigned j = 0; j < 8; ++j) {
            assert(dx[j] == oracle(INT64_C(16384)*x[j] + offsets[i]));
            assert(dy[j] == oracle(-INT64_C(16384)*x[j] + offsets[i]));
        }
    }
}

int main(void)
{
    ksn_proc_points_affine_pie((KsnProcPointDst){NULL, NULL},
                               (KsnProcPointSrc){NULL, NULL}, 0, NULL);
    extraction_boundaries();
    const KsnProcAffineQ14 edges[] = {
        {0, 0, 0, 0, 0, 0},
        {16384, 0, 0, 16384, 0, 0},
        {INT16_MIN, INT16_MAX, INT16_MAX, INT16_MIN, INT32_MIN, INT32_MAX},
        {INT16_MAX, INT16_MIN, INT16_MIN, INT16_MAX, INT32_MAX, INT32_MIN},
        {1, -1, -1, 1, -1, 1},
        {INT16_MIN, INT16_MIN, 8192, 8192, INT32_MIN, -1},
    };
    for (size_t e = 0; e < sizeof edges / sizeof edges[0]; ++e)
        for (size_t n = 0; n <= 39; ++n)
            for (int alias = 0; alias < 4; ++alias)
                check_case(n, edges[e], alias);
    /* Translation decomposition changes at each multiple of 32768; test
     * both neighbors, including the final quotient requiring three terms. */
    for (int64_t t = INT32_MIN; t <= INT32_MAX; t += INT64_C(32768)) {
        for (int delta = -1; delta <= 1; ++delta) {
            const int64_t value = t + delta;
            if (value < INT32_MIN || value > INT32_MAX) continue;
            KsnProcAffineQ14 m = {INT16_MIN, INT16_MIN,
                                  INT16_MAX, INT16_MAX,
                                  (int32_t)value, (int32_t)value};
            check_case(9, m, delta + 1);
        }
    }
    for (unsigned trial = 0; trial < 200; ++trial) {
        KsnProcAffineQ14 m = {(int16_t)next_random(), (int16_t)next_random(),
                              (int16_t)next_random(), (int16_t)next_random(),
                              (int32_t)next_random(), (int32_t)next_random()};
        check_case(next_random() % 40, m, trial & 3);
    }
    puts("proc-points-pie host contract: ok");
    return 0;
}
