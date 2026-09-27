#pragma once

// Host stub for main/vm/vm_wake.h (VM L1, docs/vm/vm-L1-design.md sec.4).
//
// The real header pulls in freertos/FreeRTOS.h and freertos/task.h, which the
// host builds do not have. It is reachable from the host because
// main/pocket/pocket_api.c (pocket_api_complete) and main/system/sys_clock.c
// include it and call vm_wake_post(). That call used to sit behind
// CONFIG_POCKET_VM_SCHED, which no host build defined; since the scheduler
// stopped being a build option (docs/vm/backlog.md, 2026-09-27) it is always
// compiled, so these are definitions, not just declarations. A host test
// drives its turns itself and has no sleeping owner task to wake, so posting
// does nothing and a wait returns at once.
//
// tools/hostshim comes before main/ on every host build's -I list, so this
// shadows the real header rather than competing with it.

#include <stdint.h>

typedef uint32_t TickType_t;

static inline void vm_wake_bind(void) {}
static inline void vm_wake_post(void) {}
static inline uint32_t vm_wake_wait(TickType_t max) { (void)max; return 0; }
