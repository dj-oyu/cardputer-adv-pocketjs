#ifndef INT_LATENCY_H
#define INT_LATENCY_H
#include <stdbool.h>
#include <stddef.h>

/* What the integer multiply, divide and shift instructions cost on THIS part,
 * and what the 64-bit library routines GCC calls for them cost: case i into
 * out as one line ("CB ..."), false past the last case. One case runs in a
 * few milliseconds, so a caller can spread them over frames. See
 * int_latency.c for the method and docs/perf/int-latency.md for results. */
bool int_latency_case(unsigned i, char *out, size_t n);
/* How many cases int_latency_case() has. */
unsigned int_latency_count(void);
#endif
