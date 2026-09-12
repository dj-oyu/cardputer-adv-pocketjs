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
  budget->start = vm_clock_now_us();
}

void vm_budget_begin(vm_budget_t *budget, int64_t limit_us) {
  vm_budget_begin_full(budget, limit_us, VM_JOB_STRIDE, VM_JOB_FLOOR,
                       limit_us > 0 ? VM_JOB_BACKSTOP : 0U);
}

void vm_budget_restart(vm_budget_t *budget) {
  if (budget != NULL)
    budget->start = (budget->clock ? budget->clock : vm_clock_now_us)();
}

vm_drain_status_t vm_sched_drain(JSRuntime *runtime, vm_budget_t *budget,
                                 unsigned *ran, JSContext **failed_ctx) {
  unsigned n = 0;
  const vm_clock_fn clock =
      (budget->clock != NULL) ? budget->clock : vm_clock_now_us;
  for (;;) {
    if (!JS_IsJobPending(runtime)) {
      *ran = n;
      return VM_DRAIN_EMPTY;
    }
    if (budget->backstop != 0U && n >= budget->backstop) {
      *ran = n;
      return VM_DRAIN_YIELDED;
    }
    /* The clock is read once per stride and never before the floor. The floor
     * is the forward-progress guarantee for a turn whose frame() already spent
     * the budget before the drain began (measured (device): frame() alone is
     * 117 ms in the worst L0 workload); the stride is what keeps the overrun
     * past the limit bounded by stride * cost-per-job rather than unbounded. */
    if (budget->limit_us > 0 && n >= budget->floor_jobs &&
        (n % budget->stride) == 0U && clock() - budget->start >= budget->limit_us) {
      *ran = n;
      return VM_DRAIN_YIELDED;
    }
    /* One job, start to finish. Nothing below this line can observe a partial
     * job, which is the whole reason the budget lives here and not inside an
     * interrupt handler. */
    const int result = JS_ExecutePendingJob(runtime, failed_ctx);
    if (result < 0) {
      *ran = n;
      return VM_DRAIN_THREW;
    }
    /* 0 means "there was nothing to run" -- JS_IsJobPending said otherwise a
     * line ago, so this cannot happen today. Treated as empty rather than
     * ignored: the alternative is spinning forever if it ever can. */
    if (result == 0) {
      *ran = n;
      return VM_DRAIN_EMPTY;
    }
    n++;
  }
}
