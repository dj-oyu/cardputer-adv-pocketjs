#include "pocketjs/vm_sched.h"

#include "quickjs.h"

void vm_budget_begin_full(vm_budget_t *budget, int64_t limit_us,
                          unsigned stride, unsigned floor_jobs,
                          unsigned backstop) {
  if (budget == NULL)
    return;
  budget->limit_us = limit_us;
  budget->stride = stride != 0U ? stride : 1U;
  budget->floor_jobs = floor_jobs;
  budget->backstop = backstop;
  budget->clock = NULL;
  budget->elapsed = 0;
  /* The ONE place microseconds become ticks. Once per turn, not once per job:
   * what the drain loop compares is two ticks. The clamp is the correctness
   * bound of an unsigned difference (see vm_sched.h) and is unreachable for
   * any budget this firmware arms -- 50 ms at 240 MHz is 12e6 ticks against a
   * ceiling of 2^31. */
  budget->ticks_per_us = vm_clock_ticks_per_us();
  if (limit_us > 0) {
    const int64_t ticks = limit_us * (int64_t)budget->ticks_per_us;
    budget->limit_ticks = ticks < (int64_t)0x80000000 ? (vm_tick_t)ticks
                                                      : (vm_tick_t)0x80000000;
  } else {
    budget->limit_ticks = 0;
  }
  budget->start = vm_clock_now();
}

void vm_budget_begin(vm_budget_t *budget, int64_t limit_us) {
  vm_budget_begin_full(budget, limit_us, VM_JOB_STRIDE, VM_JOB_FLOOR,
                       limit_us > 0 ? VM_JOB_BACKSTOP : 0U);
}

void vm_budget_restart(vm_budget_t *budget) {
  if (budget != NULL)
    budget->start = (budget->clock ? budget->clock : vm_clock_now)();
}

/* One exit for every return, so `elapsed` can never be the previous drain's
 * value: the runaway guard adds it up and a stale term there would be a
 * session ended for work that already finished. Zero in count mode, where the
 * clock is never read on purpose (the harness needs the yield points to be a
 * pure function of the program). */
static vm_drain_status_t drain_return(vm_budget_t *budget, vm_clock_fn clock,
                                      unsigned n, unsigned *ran,
                                      vm_drain_status_t status) {
  *ran = n;
  /* Ticks back to microseconds, once per drain. The subtraction is unsigned so
   * that a counter which wrapped inside this drain still yields the true
   * interval; the divide is by a small constant and costs tens of cycles
   * against a drain measured in milliseconds. */
  budget->elapsed =
      budget->limit_ticks != 0
          ? (int64_t)((vm_tick_t)(clock() - budget->start) / budget->ticks_per_us)
          : 0;
  return status;
}

vm_drain_status_t vm_sched_drain(JSRuntime *runtime, vm_budget_t *budget,
                                 unsigned *ran, JSContext **failed_ctx) {
  unsigned n = 0;
  const vm_clock_fn clock =
      (budget->clock != NULL) ? budget->clock : vm_clock_now;
  for (;;) {
    if (!JS_IsJobPending(runtime))
      return drain_return(budget, clock, n, ran, VM_DRAIN_EMPTY);
    if (budget->backstop != 0U && n >= budget->backstop)
      return drain_return(budget, clock, n, ran, VM_DRAIN_YIELDED);
    /* The clock is read once per stride and never before the floor. The floor
     * is the forward-progress guarantee for a turn whose frame() already spent
     * the budget before the drain began (measured (device): frame() alone is
     * 117 ms in the worst L0 workload); the stride is what keeps the overrun
     * past the limit bounded by stride * cost-per-job rather than unbounded. */
    if (budget->limit_ticks != 0 && n >= budget->floor_jobs &&
        (n % budget->stride) == 0U &&
        (vm_tick_t)(clock() - budget->start) >= budget->limit_ticks)
      return drain_return(budget, clock, n, ran, VM_DRAIN_YIELDED);
    /* One job, start to finish. Nothing below this line can observe a partial
     * job, which is the whole reason the budget lives here and not inside an
     * interrupt handler. */
    const int result = JS_ExecutePendingJob(runtime, failed_ctx);
    if (result < 0)
      return drain_return(budget, clock, n, ran, VM_DRAIN_THREW);
    /* 0 means "there was nothing to run" -- JS_IsJobPending said otherwise a
     * line ago, so this cannot happen today. Treated as empty rather than
     * ignored: the alternative is spinning forever if it ever can. */
    if (result == 0)
      return drain_return(budget, clock, n, ran, VM_DRAIN_EMPTY);
    n++;
  }
}
