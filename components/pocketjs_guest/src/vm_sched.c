#include "pocketjs/vm_sched.h"

#include "quickjs-vm.h"
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
    /* L2c gate (sec.12.6-4, D22r), case 1: a chain a PREVIOUS call through
     * here left parked. Checked before anything else in the loop -- even
     * JS_IsJobPending() -- because a held chain is not a job on the queue at
     * all (JS_ExecutePendingJob already list_del'd it); touching the queue
     * while it is open would run the wrong thing first. Always false in this
     * pass-through build (JS_VMSuspended has no rt->vm_susp to read yet), so
     * this is dead code until stage 3 of the design's implementation order
     * lands the real interpreter support -- kept here now so the caller-side
     * shape (this function's return value, and firmware/vmrun switching on
     * it) does not have to change again when it does. */
    if (JS_VMSuspended(runtime))
      return drain_return(budget, clock, n, ran, VM_DRAIN_SUSPENDED);
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
    /* L2c gate (sec.12.6-4, D22r), case 2 -- NOT wired here yet: a job whose
     * own handler suspended mid-chain (JS_VMCallJob's JOB_HELD path, D36)
     * would have JS_ExecutePendingJob itself return a third value (2) instead
     * of -1/0/1, at which point this call counts (n++, the job genuinely
     * ran) before returning VM_DRAIN_SUSPENDED. That needs quickjs.c's own
     * JS_ExecutePendingJob to grow that return, which is stage 3f of the
     * design's implementation order -- out of scope for the gate (stage 1)
     * and the guards (stage 2), both of which leave quickjs.c untouched. */
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
