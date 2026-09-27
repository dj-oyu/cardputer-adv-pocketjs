#include "ksn_proc_points_dispatch.h"

#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { CAP = 64 };
static unsigned pie_calls;

/* Host routing stub. The actual PIE arithmetic is checked separately; this
 * test verifies that dispatch calls it only for eligible point sets. */
void ksn_proc_points_affine_pie(KsnProcPointDst dst, KsnProcPointSrc src,
                                size_t n, const KsnProcAffineQ14 *coeff)
{
    ++pie_calls;
    ksn_proc_points_affine_scalar(dst, src, n, coeff);
}

static uint32_t random32(uint32_t *s)
{
    *s = *s * UINT32_C(1664525) + UINT32_C(1013904223);
    return *s;
}

static void expect(KsnProcPointsDecision d, KsnProcPointsBackend backend,
                   KsnProcPointsReason reason)
{
    assert(d.backend == backend);
    assert(d.reason == reason);
}

static void selection(void)
{
    _Alignas(16) int16_t sx[16], sy[16], dx[16], dy[16];
    KsnProcPointSrc src = { sx, sy };
    KsnProcPointDst dst = { dx, dy };
    const KsnProcPointsPolicy disabled = { false, 8 };
    const KsnProcPointsPolicy uncalibrated = { true, 0 };
    const KsnProcPointsPolicy enabled = { true, 12 };

    expect(ksn_proc_points_select_for_backend(dst, src, 0, &enabled, true),
           KSN_PROC_POINTS_SCALAR, KSN_PROC_POINTS_EMPTY);
    expect(ksn_proc_points_select_for_backend(dst, src, 16, NULL, true),
           KSN_PROC_POINTS_SCALAR, KSN_PROC_POINTS_DISABLED);
    expect(ksn_proc_points_select_for_backend(dst, src, 16, &disabled, true),
           KSN_PROC_POINTS_SCALAR, KSN_PROC_POINTS_DISABLED);
    expect(ksn_proc_points_select_for_backend(dst, src, 16, &uncalibrated, true),
           KSN_PROC_POINTS_SCALAR, KSN_PROC_POINTS_UNCALIBRATED);
    expect(ksn_proc_points_select_for_backend(dst, src, 16, &enabled, false),
           KSN_PROC_POINTS_SCALAR, KSN_PROC_POINTS_BACKEND_UNAVAILABLE);
    expect(ksn_proc_points_select_for_backend(dst, src, 11, &enabled, true),
           KSN_PROC_POINTS_SCALAR, KSN_PROC_POINTS_BELOW_MIN_POINTS);
    expect(ksn_proc_points_select_for_backend(dst, src, 12, &enabled, true),
           KSN_PROC_POINTS_PIE, KSN_PROC_POINTS_PIE_SELECTED);
    expect(ksn_proc_points_select_for_backend(dst, src, 16,
           &(KsnProcPointsPolicy){ true, 1 }, true),
           KSN_PROC_POINTS_PIE, KSN_PROC_POINTS_PIE_SELECTED);
    expect(ksn_proc_points_select_for_backend(dst, src, 7,
           &(KsnProcPointsPolicy){ true, 1 }, true),
           KSN_PROC_POINTS_SCALAR, KSN_PROC_POINTS_BELOW_MIN_POINTS);
    src.x++;
    expect(ksn_proc_points_select_for_backend(dst, src, 16, &enabled, true),
           KSN_PROC_POINTS_SCALAR, KSN_PROC_POINTS_UNALIGNED);
    src.x--;
    dst.y++;
    expect(ksn_proc_points_select_for_backend(dst, src, 16, &enabled, true),
           KSN_PROC_POINTS_SCALAR, KSN_PROC_POINTS_UNALIGNED);
}

static void run_case(size_t n, unsigned alias, bool enabled, uint32_t *seed)
{
    _Alignas(16) int16_t sx[CAP], sy[CAP], dx[CAP], dy[CAP];
    _Alignas(16) int16_t ex[CAP], ey[CAP];
    int16_t original_x[CAP], original_y[CAP];
    KsnProcAffineQ14 c = {
        (int16_t)(random32(seed) >> 16),
        (int16_t)(random32(seed) >> 16),
        (int16_t)(random32(seed) >> 16),
        (int16_t)(random32(seed) >> 16),
        (int32_t)random32(seed), (int32_t)random32(seed)
    };
    const KsnProcPointsPolicy policy = { enabled, 12 };
    const int16_t guard = 0x5a5a;
    assert(n < CAP);
    for (size_t i = 0; i < CAP; ++i) {
        sx[i] = (int16_t)(random32(seed) >> 16);
        sy[i] = (int16_t)(random32(seed) >> 16);
        dx[i] = dy[i] = ex[i] = ey[i] = guard;
    }
    sx[0] = INT16_MIN;
    sy[0] = INT16_MAX;
    memcpy(original_x, sx, sizeof sx);
    memcpy(original_y, sy, sizeof sy);
    ksn_proc_points_affine_scalar((KsnProcPointDst){ ex, ey },
                                  (KsnProcPointSrc){ sx, sy }, n, &c);
    KsnProcPointDst dst = { (alias & 1) ? sx : dx,
                            (alias & 2) ? sy : dy };
    unsigned before = pie_calls;
    KsnProcPointsDecision d = ksn_proc_points_affine_dispatch(
        dst, (KsnProcPointSrc){ sx, sy }, n, &c, &policy);
    bool should_pie = ksn_proc_points_pie_backend_available() &&
                      enabled && n >= 12;
    assert(d.backend == (should_pie ? KSN_PROC_POINTS_PIE :
                                      KSN_PROC_POINTS_SCALAR));
    assert(pie_calls == before + (unsigned)should_pie);
    for (size_t i = 0; i < n; ++i) {
        assert(dst.x[i] == ex[i]);
        assert(dst.y[i] == ey[i]);
    }
    for (size_t i = n; i < CAP; ++i) {
        assert(dst.x[i] == ((alias & 1) ? original_x[i] : guard));
        assert(dst.y[i] == ((alias & 2) ? original_y[i] : guard));
    }
    if (!(alias & 1)) assert(memcmp(sx, original_x, sizeof sx) == 0);
    if (!(alias & 2)) assert(memcmp(sy, original_y, sizeof sy) == 0);
}

int main(void)
{
    const size_t counts[] = { 1, 7, 8, 9, 11, 12, 15, 16, 17, 23, 32, 47 };
    const KsnProcPointsPolicy enabled = { true, 8 };
    uint32_t seed = UINT32_C(0x8bb91864);
    selection();
    expect(ksn_proc_points_affine_dispatch(
               (KsnProcPointDst){ NULL, NULL },
               (KsnProcPointSrc){ NULL, NULL }, 0, NULL, &enabled),
           KSN_PROC_POINTS_SCALAR, KSN_PROC_POINTS_EMPTY);
    assert(pie_calls == 0);
    for (unsigned repeat = 0; repeat < 20; ++repeat)
        for (size_t i = 0; i < sizeof counts / sizeof counts[0]; ++i)
            for (unsigned alias = 0; alias < 4; ++alias)
                for (unsigned enable = 0; enable < 2; ++enable)
                    run_case(counts[i], alias, enable != 0, &seed);
    puts("proc-points-dispatch: ok");
    return 0;
}
