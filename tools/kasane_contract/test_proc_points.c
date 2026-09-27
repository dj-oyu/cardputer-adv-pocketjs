#include "ksn_proc_points.h"

#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { CAPACITY = 48 };

/* Independent oracle: ceil the magnitude for negative values, then negate. */
static int16_t expected(int64_t numerator)
{
    int64_t q;
    if (numerator < 0) {
        q = -((-numerator + INT64_C(16383)) / INT64_C(16384));
    } else {
        q = numerator / INT64_C(16384);
    }
    if (q < INT16_MIN) return INT16_MIN;
    if (q > INT16_MAX) return INT16_MAX;
    return (int16_t)q;
}

static uint32_t next_random(uint32_t *state)
{
    *state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
    return *state;
}

static void run_case(size_t n, const KsnProcAffineQ14 *c, int in_place,
                     uint32_t *seed)
{
    _Alignas(16) int16_t sx[CAPACITY], sy[CAPACITY];
    _Alignas(16) int16_t dx[CAPACITY], dy[CAPACITY];
    int16_t original_x[CAPACITY], original_y[CAPACITY];
    const int16_t guard = (int16_t)0x6a5b;

    assert(n < CAPACITY);
    for (size_t i = 0; i < CAPACITY; ++i) {
        sx[i] = guard;
        sy[i] = guard;
        dx[i] = guard;
        dy[i] = guard;
    }
    for (size_t i = 0; i < n; ++i) {
        sx[i] = (int16_t)(next_random(seed) >> 16);
        sy[i] = (int16_t)(next_random(seed) >> 16);
    }
    /* Force all coordinate extremes and negative fractional results into
     * every sufficiently long block, independent of random coverage. */
    if (n > 0) { sx[0] = INT16_MIN; sy[0] = INT16_MAX; }
    if (n > 1) { sx[1] = -1; sy[1] = 1; }
    if (n > 2) { sx[2] = INT16_MAX; sy[2] = INT16_MIN; }
    memcpy(original_x, sx, sizeof sx);
    memcpy(original_y, sy, sizeof sy);

    KsnProcPointSrc src = { sx, sy };
    KsnProcPointDst dst = { (in_place & 1) ? sx : dx,
                           (in_place & 2) ? sy : dy };
    ksn_proc_points_affine_scalar(dst, src, n, c);

    for (size_t i = 0; i < n; ++i) {
        int64_t x = original_x[i], y = original_y[i];
        int64_t nx = (int64_t)c->m00 * x + (int64_t)c->m01 * y + c->tx;
        int64_t ny = (int64_t)c->m10 * x + (int64_t)c->m11 * y + c->ty;
        assert(dst.x[i] == expected(nx));
        assert(dst.y[i] == expected(ny));
    }
    for (size_t i = n; i < CAPACITY; ++i) {
        assert(dst.x[i] == guard);
        assert(dst.y[i] == guard);
    }
    if (!(in_place & 1)) assert(memcmp(sx, original_x, sizeof sx) == 0);
    if (!(in_place & 2)) assert(memcmp(sy, original_y, sizeof sy) == 0);
    for (size_t i = 0; i < CAPACITY; ++i) {
        if (in_place & 1) assert(dx[i] == guard);
        if (in_place & 2) assert(dy[i] == guard);
    }
}

static void cancellation(void)
{
    _Alignas(16) int16_t x[8] = { INT16_MIN, 1 };
    _Alignas(16) int16_t y[8] = { INT16_MIN, 1 };
    _Alignas(16) int16_t dx[8], dy[8];
    /* Two products sum to 2^31, then translation cancels them exactly.
     * A premature signed-32 or signed-16 saturation loses the zero. */
    KsnProcAffineQ14 c = { INT16_MIN, INT16_MIN, 8192, 8192,
                          INT32_MIN, 0 };
    ksn_proc_points_affine_scalar((KsnProcPointDst){dx,dy},
                                 (KsnProcPointSrc){x,y},2,&c);
    assert(dx[0]==0 && dy[0]==INT16_MIN);
    /* Each half is fractional; narrowing products before summing gives 0. */
    assert(dy[1]==1);
}

int main(void)
{
    const size_t counts[] = { 0, 1, 7, 8, 9, 15, 16, 17, 32, 35 };
    const KsnProcAffineQ14 matrices[] = {
        { 16384, 0, 0, 16384, 0, 0 },
        { 8192, -8192, 4096, 12288, -1, 1 },
        { INT16_MIN, INT16_MAX, INT16_MAX, INT16_MIN, INT32_MIN, INT32_MAX },
        { INT16_MAX, INT16_MAX, INT16_MIN, INT16_MIN, INT32_MAX, INT32_MIN },
        { 0, 0, 0, 0, -1, 16383 },
    };
    uint32_t seed = UINT32_C(0x12ab34cd);

    ksn_proc_points_affine_scalar((KsnProcPointDst){ NULL, NULL },
                                   (KsnProcPointSrc){ NULL, NULL }, 0, NULL);
    for (size_t m = 0; m < sizeof matrices / sizeof matrices[0]; ++m) {
        for (size_t k = 0; k < sizeof counts / sizeof counts[0]; ++k) {
            for (int alias = 0; alias < 4; ++alias)
                run_case(counts[k], &matrices[m], alias, &seed);
        }
    }
    cancellation();
    puts("proc-points: ok");
    return 0;
}
