#ifndef KSN_PROC_POINTS_H
#define KSN_PROC_POINTS_H

#include <stddef.h>
#include <stdint.h>

/* Independent 2D points, stored as two int16_t planes (SoA). Each coordinate
 * is an integer in [-32768, 32767], with no implicit conversion from the
 * existing float VM. Matrix entries are signed Q2.14, i.e. [-2, 2), and
 * translations are signed int32_t Q14. For each point:
 *
 *   X = m00*x + m01*y + tx;  Y = m10*x + m11*y + ty;
 *   out_x = sat16(floor(X / 16384));
 *   out_y = sat16(floor(Y / 16384)).
 *
 * Products and each complete sum use signed int64_t; no intermediate result
 * wraps or saturates. The sum fits in signed 34 bits for all legal values.
 * Negative division rounds toward minus infinity, independent of a C
 * implementation's signed-right-shift behavior. Only the final coordinates
 * saturate to [-32768, 32767].
 *
 * For n > 0, all four plane bases and coeff must be valid. Every plane base
 * must be 16-byte aligned and hold at least n int16_t elements. The x and y
 * source planes are disjoint, as are the destination planes. A destination
 * may equal its corresponding source base exactly (in-place); any other
 * source/destination overlap is unsupported. Thus both inputs of point i
 * can be read before writing either output of i. coeff must not overlap a
 * destination plane. For n == 0, all pointers may be NULL and nothing is
 * read or written. The only writes are dst.x[0..n) and dst.y[0..n), including
 * a scalar tail of 1..7 points. No padding or guard element is touched.
 *
 * The same contract is intended for a future eight-lane PIE replacement:
 * 16-byte aligned bases permit 8 x int16_t loads/stores. This scalar entry
 * point does not use PIE and makes no device performance claim.
 * A PIE implementation must accumulate the full sum before extracting or
 * saturating; narrowing each product first changes fractional cancellation.
 * The 34-bit bound fits the 40-bit QACC. Translation must likewise enter the
 * accumulator at full precision, not be narrowed to a 16-bit lane. Reload
 * or recompute QACC for each output: extraction may modify its contents.
 */
typedef struct {
    const int16_t *x;
    const int16_t *y;
} KsnProcPointSrc;

typedef struct {
    int16_t *x;
    int16_t *y;
} KsnProcPointDst;

typedef struct {
    int16_t m00, m01, m10, m11;
    int32_t tx, ty;
} KsnProcAffineQ14;

void ksn_proc_points_affine_scalar(KsnProcPointDst dst,
                                   KsnProcPointSrc src, size_t n,
                                   const KsnProcAffineQ14 *coeff);

#endif
