// Are flower.c's ifloor/iceil the library's floorf/ceilf, for every float?
//
// They replace `(int)floorf(x)` and `(int)ceilf(x)` on flower.c's hot paths,
// where the call is not an optimisation detail but the whole cost: floorf on
// this part is `l32r`+`callx8` into a ROM routine, and the cast it replaces it
// with is one `trunc.s` (docs/pie-simd.md 3.7).
//
// docs/flower-perf-handoff.md proposed a bare `(int)` cast justified by an
// argument that the argument is never negative, and named the one way that
// breaks: a longitudinal coordinate that rounds a hair below -1 makes
// (longitudinal+1)*4 slightly negative, floorf says -1, the cast says 0, and
// the chequer's parity inverts for that pixel. The argument is sound and the
// handoff is right that the work is the proof rather than the edit -- so this
// takes the other road and removes the precondition instead. ifloor corrects
// the truncation's direction unconditionally, which makes the claim one about
// the representation rather than about the values, and a claim about the
// representation can be settled by exhaustion.
//
// So it is settled by exhaustion. All 2^32 bit patterns, against the library,
// skipping only what neither side defines: NaN, infinity, and magnitudes that
// do not fit in an int (both sides are undefined there, and flower.c's callers
// clamp into a screen rectangle long before that).
//
//   gcc -O2 -Wall -Wextra -Werror tools/test_flower_floor.c -lm -o /tmp/ff && /tmp/ff
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

static inline int ifloor(float x) { int t=(int)x;return t-(x<0.0f&&(float)t!=x); }
static inline int iceil (float x) { int t=(int)x;return t+(x>0.0f&&(float)t!=x); }

int main(void) {
    unsigned long checked=0,skipped=0;
    uint32_t u=0;
    do {
        float x;memcpy(&x,&u,4);
        // isfinite excludes NaN and infinity; the magnitude test excludes the
        // range where the C cast itself is undefined behaviour. 2^31 exactly is
        // already out, so the bound is one representable step below it.
        if(!isfinite(x)||fabsf(x)>=2147483520.0f) { skipped++;continue; }
        int fl=(int)floorf(x),ce=(int)ceilf(x);
        assert(ifloor(x)==fl);
        assert(iceil(x)==ce);
        checked++;
    } while(++u!=0);
    printf("FLOOR_OK: %lu/%lu float bit patterns agree with floorf and ceilf"
           " (%lu skipped: NaN, infinity, or outside int)\n",
           checked,checked+skipped,skipped);
    return 0;
}
