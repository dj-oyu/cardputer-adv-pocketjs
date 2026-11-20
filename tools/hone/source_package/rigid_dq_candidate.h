#ifndef HONE_RIGID_DQ_CANDIDATE_H
#define HONE_RIGID_DQ_CANDIDATE_H
#include <stddef.h>
/* Prepared data is caller-owned, immutable during all vertex calls, and must
 * come from successful prepare calls for this same pose. No generation cache. */
typedef struct { double rotation[4], translation_product[4]; } rigid_dq_joint;
/* One joint commits only on success. Stage a whole palette before publishing.
 * Translation product is deliberately NOT premultiplied by .5: preserve the
 * original per-vertex multiplication order and rounding. */
int rigid_dq_prepare_joint(const double joint[7], rigid_dq_joint *out);
int rigid_dq_position(const double *,const double *,size_t,const unsigned *,
                      const double *,size_t,double *);
int rigid_dq_position_prepared(const double *,const rigid_dq_joint *,size_t,
                               const unsigned *,const double *,size_t,double *);
#endif
