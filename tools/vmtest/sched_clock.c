// Deadline uses turn start; elapsed must exclude work before this drain.
#include <assert.h>
#include <stdio.h>
#include "pocketjs/vm_sched.h"
#include "quickjs.h"

static vm_tick_t ticks;
static unsigned reads;
static vm_tick_t fake_clock(void) { reads++; ticks += 100; return ticks; }
bool JS_IsJobPending(JSRuntime *rt) { (void)rt; return false; }
int JS_VMSuspended(JSRuntime *rt) { (void)rt; return 0; }
int JS_ExecutePendingJob(JSRuntime *rt, JSContext **ctx) { (void)rt; (void)ctx; return 0; }

int main(void) {
    vm_budget_t budget;
    vm_budget_begin_full(&budget, 10000, 1, 0, 0);
    budget.clock = fake_clock;
    budget.ticks_per_us = 1;
    budget.start = 100;
    ticks = 1000;
    unsigned ran = 99;
    JSContext *ctx = NULL;
    assert(vm_sched_drain(NULL, &budget, &ran, &ctx) == VM_DRAIN_EMPTY);
    assert(ran == 0 && budget.elapsed == 100 && reads == 2);
    ticks = UINT32_MAX - 150;
    assert(vm_sched_drain(NULL, &budget, &ran, &ctx) == VM_DRAIN_EMPTY);
    assert(budget.elapsed == 100);
    budget.limit_ticks = 0;
    reads = 0;
    assert(vm_sched_drain(NULL, &budget, &ran, &ctx) == VM_DRAIN_EMPTY);
    assert(budget.elapsed == 0 && reads == 0);
    puts("sched clock: call-relative, wrap, count-mode OK");
}
