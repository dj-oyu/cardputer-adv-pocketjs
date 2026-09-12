#pragma once

// Host stub for main/vm/vm_wake.h (VM L1, docs/vm-L1-design.md sec.4).
//
// The real header pulls in freertos/FreeRTOS.h and freertos/task.h, which the
// host builds do not have. It is reachable from the host only because
// main/pocket/pocket_api.c includes it, and there the ONLY use --
// vm_wake_post() at the end of pocket_api_complete() -- sits behind
// #ifdef CONFIG_POCKET_VM_SCHED, which no host build defines (there is no
// sdkconfig.h on tools/hostshim). So a host build needs the name to resolve
// and nothing else: these declarations exist to be compiled, not linked.
//
// tools/hostshim comes before main/ on every host build's -I list, so this
// shadows the real header rather than competing with it.

#include <stdint.h>

typedef uint32_t TickType_t;

void vm_wake_bind(void);
void vm_wake_post(void);
uint32_t vm_wake_wait(TickType_t max);
