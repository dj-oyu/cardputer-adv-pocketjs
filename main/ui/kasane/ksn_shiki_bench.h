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
#endif
