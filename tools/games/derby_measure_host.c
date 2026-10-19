/* Functional-accounting fixture only: host oracle/debug redraws are included.
 * This is NOT a device or production-path performance benchmark. */
#include "derby_measure.h"
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <time.h>
#include <inttypes.h>
uint64_t dm_now_us(void){
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000u+(uint64_t)t.tv_nsec/1000u;
}
__attribute__((constructor)) static void begin(void){dm_reset();dm_calibrate();}
__attribute__((destructor)) static void finish(void){
    dm_snapshot s;if(!dm_read(&s)){fputs("MEASURE_HOST unbalanced scopes\n",stderr);fflush(stderr);_Exit(EXIT_FAILURE);}
    assert(!s.faults);
    fprintf(stderr,"MEASURE_HOST functional_only=1 faults=%" PRIu64 " scalar=%" PRIu64 " pie=%" PRIu64 "\n",
            s.faults,s.scalar_batches,s.pie_batches);
    for(unsigned i=0;i<DM_STAGES;i++)fprintf(stderr,"MEASURE_HOST s%u=%" PRIu64 ":%" PRIu64 ":%" PRIu64 "\n",
        i,s.stage[i].inclusive_us,s.stage[i].self_us,s.stage[i].calls);
}
