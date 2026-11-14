#ifndef EDGE_BENCH_CORE_H
#define EDGE_BENCH_CORE_H
#include <stdint.h>
#include <stdbool.h>
#define EDGE_BENCH_ROUNDS 31
/* caller uses normal pinned task with CP3 context, never ISR. No device writes. */
typedef uint32_t (*edge_bench_clock)(void);
typedef struct{uint32_t scalar[31],pie[31],clock_overhead[31];uint64_t checksum;unsigned completed;int error;} edge_bench_result;
int edge_bench_run(edge_bench_clock clock,void (*yield_hook)(void),edge_bench_result*out);
typedef struct { bool (*cancelled)(void*); void (*preflight_yield)(void*); void *context; } edge_bench_control;
int edge_bench_run_controlled(edge_bench_clock,void (*)(void),edge_bench_result*,const edge_bench_control*);
#endif
