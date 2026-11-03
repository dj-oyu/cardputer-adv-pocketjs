#ifndef KSN_SHIKI_BENCH_H
#define KSN_SHIKI_BENCH_H
/* Paired workloads for the PIE prototype (docs/kasane/shiki-pie.md sec.6):
 * each pair is the same drawing written once with v1.3's scalar data
 * instructions and once with the prototype's vector / batch instructions.
 * The host test (tools/kasane_contract/test_shiki_vec.c) proves each pair
 * emits the same segments; the device bench (pocket_shiki.c bench()) times
 * them in one binary. Nothing here allocates: the caller hands work memory. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ksn_shiki.h"

/* int16 elements the caller provides for a case's regions. */
#define KSN_SHIKI_BENCH_WORK 1024

typedef struct {
    const char *name;
    uint8_t code[12+4*KSN_SHIKI_REGIONS+12*KSN_SHIKI_CODE];
    size_t len;
    ksn_shiki_region region[KSN_SHIKI_REGIONS];
    unsigned regions;
    unsigned pair;      /* cases with the same pair draw the same segments */
} ksn_shiki_bench_case;

unsigned ksn_shiki_bench_count(void);
/* Builds case i into *c with its regions in work (filled with the case's
 * data). False past the last case. */
bool ksn_shiki_bench_build(unsigned i,int16_t work[KSN_SHIKI_BENCH_WORK],ksn_shiki_bench_case *c);
/* Programs that are not timed: each runs once on the C model and once on
 * PIE (ksn_shiki_vec_pie) and every observable must agree (segments, steps,
 * registers, vector registers, region contents). They aim at what the TRM
 * leaves open or the prototype assumes: VUNZIP's lane order, QACC's image
 * round trip, VMULAS's 40-bit saturation, what SRCMB writes back, shifts of
 * 32 and more, the tail and unaligned fallbacks. work must be 16-B aligned. */
unsigned ksn_shiki_bench_check_count(void);
bool ksn_shiki_bench_check_build(unsigned i,int16_t work[KSN_SHIKI_BENCH_WORK],ksn_shiki_bench_case *c);
/* A random program for the scalar JIT's checks (docs/kasane/shiki-jit.md
 * sec.11): well nested, up to 64 instructions of the scalar set (arithmetic
 * at the int32 boundaries, loops up to 255 deep enough to reach the step
 * limit, BREAK_IF_GT, MOVE/LINE/PLOT, SIN, ADD_H, DRAW_INDEX), no regions,
 * its inputs and draw index. Registration may still refuse one (a
 * coordinate it checks): draw another. The host test
 * (tools/shiki_jit/test_scalar.c) and the device bench run the same ones. */
size_t ksn_shiki_bench_random(uint32_t *seed,ksn_shiki_bench_case *c,int32_t in[KSN_SHIKI_INPUTS],
                              unsigned *nin,unsigned *index);
#endif
