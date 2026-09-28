#include "ksn_proc_points.h"

#include <limits.h>

static int16_t round_and_saturate(int64_t sum)
{
    /* C division truncates toward zero. Adjust the negative fractional case
     * explicitly to implement mathematical floor without a signed shift. */
    int64_t whole = sum / INT64_C(16384);
    if (sum < 0 && sum % INT64_C(16384) != 0) {
        --whole;
    }
    if (whole > INT16_MAX) {
        return INT16_MAX;
    }
    if (whole < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)whole;
}

void ksn_proc_points_affine_scalar(KsnProcPointDst dst,
                                   KsnProcPointSrc src, size_t n,
                                   const KsnProcAffineQ14 *coeff)
{
    for (size_t i = 0; i < n; ++i) {
        const int64_t x = src.x[i];
        const int64_t y = src.y[i];
        const int64_t out_x = (int64_t)coeff->m00 * x +
                              (int64_t)coeff->m01 * y + coeff->tx;
        const int64_t out_y = (int64_t)coeff->m10 * x +
                              (int64_t)coeff->m11 * y + coeff->ty;
        dst.x[i] = round_and_saturate(out_x);
        dst.y[i] = round_and_saturate(out_y);
    }
}
