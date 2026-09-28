#include "ksn_proc_points_pie.h"

#include <stdint.h>

#if KSN_PROC_POINTS_HAS_PIE || defined(KSN_PROC_POINTS_PIE_MODEL)

/* Translation representation: t = r - 32768*(a+b+c). C's signed remainder
 * gives |r| <= 32767. The quotient of a signed int32 by 32768 has magnitude
 * at most 65536, so three signed 16-bit summands suffice. Each lane of QACC
 * receives this exact t before either coordinate product. */
static void translation_terms(int32_t t, int16_t out[4])
{
    int64_t q = -(int64_t)(t / INT32_C(32768));
    out[0] = (int16_t)(t % INT32_C(32768));
    for (unsigned j = 1; j < 4; ++j) {
        int64_t part = q;
        if (part > INT16_MAX) part = INT16_MAX;
        if (part < INT16_MIN) part = INT16_MIN;
        out[j] = (int16_t)part;
        q -= part;
    }
}

/* Order follows the post-increment broadcasts below. */
typedef struct __attribute__((aligned(16))) {
    int16_t neg_32768;
    int16_t xr, xa, xb, xc, m00, m01;
    int16_t yr, ya, yb, yc, m10, m11;
} PieTerms;

_Static_assert(offsetof(PieTerms, m01) == 12, "PIE x broadcast layout");
_Static_assert(offsetof(PieTerms, yr) == 14, "PIE y broadcast layout");
_Static_assert(offsetof(PieTerms, m11) == 24, "PIE final broadcast layout");

#if KSN_PROC_POINTS_HAS_PIE
__attribute__((noinline))
static void block8(int16_t *dx, int16_t *dy,
                   const int16_t *sx, const int16_t *sy,
                   const PieTerms *terms)
{
    const int16_t *p = &terms->neg_32768;
    const int shift = 14;
    /* The Xtensa GCC backend does not name q registers in asm clobber lists
     * (it rejects "q0" etc.). This noinline call owns q0..q5; GCC does not
     * allocate PIE registers for C expressions. "memory" fences the loads and
     * stores, including when a destination is an in-place source. */
    __asm__ volatile(
        "ee.vld.128.ip q0, %[sx], 0\n"
        "ee.vld.128.ip q1, %[sy], 0\n"
        "ee.vldbc.16.ip q3, %[p], 2\n"
        "ee.vldbc.16.ip q2, %[p], 2\n"
        "ee.vldbc.16.ip q5, %[p], 2\n"
        "ee.mov.s16.qacc q2\n"
        "ee.vldbc.16.ip q2, %[p], 2\n"
        "ee.vmulas.s16.qacc q5, q3\n"
        "ee.vldbc.16.ip q5, %[p], 2\n"
        "ee.vmulas.s16.qacc q2, q3\n"
        "ee.vldbc.16.ip q2, %[p], 2\n"
        "ee.vmulas.s16.qacc q5, q3\n"
        "ee.vldbc.16.ip q5, %[p], 2\n"
        "ee.vmulas.s16.qacc q0, q2\n"
        "ee.vmulas.s16.qacc q1, q5\n"
        "ee.srcmb.s16.qacc q4, %[shift], 0\n"
        "ee.vldbc.16.ip q2, %[p], 2\n"
        "ee.vst.128.ip q4, %[dx], 0\n"
        "ee.vldbc.16.ip q5, %[p], 2\n"
        "ee.mov.s16.qacc q2\n"
        "ee.vldbc.16.ip q2, %[p], 2\n"
        "ee.vmulas.s16.qacc q5, q3\n"
        "ee.vldbc.16.ip q5, %[p], 2\n"
        "ee.vmulas.s16.qacc q2, q3\n"
        "ee.vldbc.16.ip q2, %[p], 2\n"
        "ee.vmulas.s16.qacc q5, q3\n"
        "ee.vldbc.16.ip q5, %[p], 2\n"
        "ee.vmulas.s16.qacc q0, q2\n"
        "ee.vmulas.s16.qacc q1, q5\n"
        "ee.srcmb.s16.qacc q4, %[shift], 0\n"
        "ee.vst.128.ip q4, %[dy], 0\n"
        : [p] "+&a"(p)
        : [sx] "a"(sx), [sy] "a"(sy), [dx] "a"(dx), [dy] "a"(dy),
          [shift] "a"(shift)
        : "memory");
}
#else
/* Host arithmetic model exercises the actual translation decomposition and
 * block/tail traversal. It does not emulate or validate PIE instructions. */
static int16_t model_extract(int64_t sum)
{
    int64_t q = sum / INT64_C(16384);
    if (sum < 0 && sum % INT64_C(16384)) --q;
    if (q < INT16_MIN) return INT16_MIN;
    if (q > INT16_MAX) return INT16_MAX;
    return (int16_t)q;
}

static void block8(int16_t *dx, int16_t *dy,
                   const int16_t *sx, const int16_t *sy,
                   const PieTerms *t)
{
    const int64_t tx = t->xr + (int64_t)t->neg_32768 *
                             ((int64_t)t->xa + t->xb + t->xc);
    const int64_t ty = t->yr + (int64_t)t->neg_32768 *
                             ((int64_t)t->ya + t->yb + t->yc);
    for (unsigned i = 0; i < 8; ++i) {
        const int64_t x = sx[i], y = sy[i];
        dx[i] = model_extract(tx + t->m00*x + t->m01*y);
        dy[i] = model_extract(ty + t->m10*x + t->m11*y);
    }
}
#endif

void ksn_proc_points_affine_pie(KsnProcPointDst dst,
                                KsnProcPointSrc src, size_t n,
                                const KsnProcAffineQ14 *coeff)
{
    if (n == 0) return;
    PieTerms t;
    t.neg_32768 = INT16_MIN;
    int16_t x[4], y[4];
    translation_terms(coeff->tx, x);
    translation_terms(coeff->ty, y);
    t.xr = x[0]; t.xa = x[1]; t.xb = x[2]; t.xc = x[3];
    t.m00 = coeff->m00; t.m01 = coeff->m01;
    t.yr = y[0]; t.ya = y[1]; t.yb = y[2]; t.yc = y[3];
    t.m10 = coeff->m10; t.m11 = coeff->m11;
    const size_t blocks = n / 8;
    for (size_t b = 0; b < blocks; ++b) {
        const size_t i = b * 8;
        block8(dst.x + i, dst.y + i, src.x + i, src.y + i, &t);
    }
    const size_t tail = n - blocks * 8;
    if (tail) {
        KsnProcPointDst d = { dst.x + blocks * 8, dst.y + blocks * 8 };
        KsnProcPointSrc s = { src.x + blocks * 8, src.y + blocks * 8 };
        ksn_proc_points_affine_scalar(d, s, tail, coeff);
    }
}

#else

void ksn_proc_points_affine_pie(KsnProcPointDst dst,
                                KsnProcPointSrc src, size_t n,
                                const KsnProcAffineQ14 *coeff)
{
    ksn_proc_points_affine_scalar(dst, src, n, coeff);
}

#endif
