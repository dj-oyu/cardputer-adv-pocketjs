#pragma once

/* VM L1 (docs/vm-L1-design.md sec.1): a Promise job queue drain that can stop
 * BETWEEN jobs and be resumed by a later call.
 *
 * The whole point of the level is where the check sits. JS_ExecutePendingJob()
 * takes one job off the queue, runs job_func to completion and returns
 * (ledger 03 fact 7), so a decision taken between two of those calls never
 * observes a half-finished job: no skipped finally, no await left pending
 * forever, no uncatchable InternalError. That is the only such point the VM
 * offers without being modified, and L1's completion condition is that
 * quickjs.c is not modified.
 *
 * Time is the primary limit and the counts are the scaffolding around it: the
 * stride bounds how far past the limit one check can let the drain run, the
 * floor guarantees forward progress on a turn where frame() alone already
 * spent the budget, and the backstop is what stops the loop if the clock is
 * dead. Measured (device, L0 sec.2.1) job costs differ by 7x between
 * workloads -- 0.49 ms/job for the async-generator case against 0.07 ms/job
 * for the Promise chain -- which is why a pure job count cannot be the limit.
 *
 * No esp_err.h, no sdkconfig.h, no esp headers at all: tools/vmtest/vmrun.c
 * LINKS this file instead of copying it, so the harness tests the scheduler
 * the firmware ships rather than a re-implementation of it. */

#include <stdbool.h>
#include <stdint.h>

#include "pocketjs/vm_clock.h"

#ifdef __cplusplus
extern "C" {
#endif

/* quickjs.h is not included here on purpose (see the header comment); these
 * repeat its typedefs, which C11 permits in a translation unit that has both. */
typedef struct JSRuntime JSRuntime;
typedef struct JSContext JSContext;

/* Defaults, from docs/vm-L1-design.md sec.1.2. Each is justified there against
 * a measured (device) number; none of them is a guess dressed as a constant. */
#define VM_TURN_BUDGET_US 8000  /* 33.3 ms frame - 7.7 ms transfer - 1.5 ms UI, /3 */
#define VM_JOB_STRIDE 4         /* overrun past the limit <= stride * cost-per-job */
#define VM_JOB_FLOOR 8          /* runs even when frame() already spent the turn */
#define VM_JOB_BACKSTOP 64      /* only reachable with a dead clock; max drain was 68 */
/* The Back turn (main.c calls app_tick(0x2000) once to give the guest a last
 * chance to save) gets room to finish instead of being cut: the session ends
 * immediately after it, so no reordering it causes is observable. */
#define VM_LEAVE_BUDGET_US 50000
#define VM_LEAVE_BACKSTOP 256
/* Runaway is a property of ONE LOGICAL DRAIN (the drain the budget cut plus
 * every continuation of it, sec.2.1), not of a turn count.
 *
 * Counting turns was the first design and it was wrong: the backstop ends a
 * turn at 64 jobs however cheap they are, so "30 turns" is "1,920 jobs" for
 * anything cheaper than 125 us a job -- and the measured (device) Promise
 * chain is 0.07 ms a job, i.e. 134 ms of JS where the guard it replaced
 * allowed 250 ms. An honest 2,500-job chain died of it
 * (tools/vmtest/known/runaway_vs_honest_chain.js). Nor is a turn a unit of
 * guest work: the time budget that ends a turn is WALL CLOCK, so the turns of
 * a contended machine are short for reasons the guest has nothing to do with,
 * and counting them charges the guest for the audio task.
 *
 * So the guard totals what the drain actually spent. VM_RUNAWAY_US is summed
 * over vm_sched_drain() calls only -- not frame(), not the pumps, not the
 * render, not the transfer -- which makes it strictly more permissive than the
 * 250 ms wall-clock deadline it replaces: a chain the old guard let live
 * cannot be ended by this one. Preemption inside the drain is still charged,
 * because no host-side mechanism can tell that time apart from JS time; the
 * answer to that is the size of the allowance, not a smaller unit.
 *
 * VM_RUNAWAY_JOBS is the dead-clock fallback and nothing else: it is what
 * stops `function f(){Promise.resolve().then(f)}` when the clock abstraction
 * returns a constant (count mode on a host, a broken timer on a board). It is
 * far above any honest drain -- 250 ms at the measured cheapest job is ~3,600
 * jobs -- so on a live clock the time limit always fires first. */
#define VM_RUNAWAY_US 250000
#define VM_RUNAWAY_JOBS 100000

typedef enum {
  VM_DRAIN_EMPTY = 0,   /* JS_IsJobPending() went false: a real end of drain */
  VM_DRAIN_YIELDED = 1, /* budget spent, jobs remain, nothing was dropped */
  VM_DRAIN_THREW = 2,   /* a job threw; the rest of the queue stays queued */
} vm_drain_status_t;

typedef struct {
  /* Whatever vm_clock_fn returns, read once at turn start. ONLY DIFFERENCES
   * ARE USED, so a 32-bit source that wraps (CCOUNT wraps every ~17.9 s at
   * 240 MHz) is correct for any turn shorter than its period. */
  int64_t start;
  /* What the last vm_sched_drain() on this budget spent, in the clock's units.
   * Written on every return, and zero in count mode (limit_us <= 0) where the
   * clock is deliberately never read. The runaway guard sums it across one
   * logical drain; it costs one extra clock read per drain (measured: 25 ns
   * CCOUNT, 833 ns systimer) against a drain measured in milliseconds. */
  int64_t elapsed;
  int64_t limit_us;   /* <= 0 disables the time check entirely (count mode) */
  unsigned stride;    /* clock reads happen every `stride` jobs; 0 means 1 */
  unsigned floor_jobs;
  unsigned backstop;  /* 0 means no count ceiling */
  vm_clock_fn clock;  /* NULL uses vm_clock_now_us() */
} vm_budget_t;

/** Arm a budget with the L1 defaults and read the clock once. `limit_us <= 0`
 * builds an unlimited budget: the drain then behaves exactly like the
 * pre-L1 one, which is what the Kconfig switch selects. */
void vm_budget_begin(vm_budget_t *budget, int64_t limit_us);

/** Arm a budget with explicit values (the leave turn, and the harness's
 * deterministic count mode). Reads the clock once. */
void vm_budget_begin_full(vm_budget_t *budget, int64_t limit_us,
                          unsigned stride, unsigned floor_jobs,
                          unsigned backstop);

/** Re-read the clock into `start` without changing the limits. A continuation
 * turn gets a fresh budget of the same shape. */
void vm_budget_restart(vm_budget_t *budget);

/** Run jobs until the queue empties, the budget is spent, or a job throws.
 * `*ran` is set to the number of jobs completed by THIS call (always exact;
 * a job that threw is not counted, matching the pre-L1 loop). `failed_ctx`
 * receives JS_ExecutePendingJob's context out-parameter. */
vm_drain_status_t vm_sched_drain(JSRuntime *runtime, vm_budget_t *budget,
                                 unsigned *ran, JSContext **failed_ctx);

#ifdef __cplusplus
}
#endif
