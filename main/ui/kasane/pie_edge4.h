#ifndef PRIVATE_PIE_EDGE4_H
#define PRIVATE_PIE_EDGE4_H
#include <stdint.h>
#include <stdbool.h>
/* Shared private host/target experiment interface; not production integration.
 * Original finite packet/area/bounds validation remains caller responsibility.
 * q coefficients are signed canonical edges after multiplying original sign. */
enum { PIE_EDGE4_LIMIT=190 };
typedef struct { int32_t abc[3][3]; } pie_edge4_coeff;
typedef struct { _Alignas(16) int32_t v[3][4]; } pie_edge4_lanes;
typedef struct { _Alignas(16) uint32_t v[4]; } pie_edge4_mask;
/* false preserves out. No pointers may overlap. Setup only, once per face. */
bool pie_edge4_prepare(const double signed_abc[3][3],pie_edge4_coeff *out);
/* Valid x0..239,y0..134, initializes padded lanes through x+3. */
bool pie_edge4_init(const pie_edge4_coeff *,unsigned x,unsigned y,pie_edge4_lanes *out);
/* Raw classifier: aligned16, disjoint valid input/output. FFFFFFFF = safe reject.
 * q0..q5 destroyed on S3; QACC untouched. External non-LTO leaf, normal pinned
 * task with verified IDF CP3 preservation only; no ISR/kernel/manual CPENABLE.
 * Other asm cannot hold q state across the call. Host version is scalar model.
 * Final block can have tail lanes; caller must ignore lanes beyond right bound. */
void pie_edge4_reject_raw(const pie_edge4_lanes *,pie_edge4_mask *);
/* Advance all lanes by4A only after admitted prepare/init, at most throughx246.
 * Caller's row loop bounds establish no further speculative advance. */
static inline void pie_edge4_advance(const pie_edge4_coeff *q,pie_edge4_lanes *v){
 for(unsigned k=0;k<3;k++)for(unsigned i=0;i<4;i++)v->v[k][i]+=4*q->abc[k][0];
}
#endif
