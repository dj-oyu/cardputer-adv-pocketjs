#include "pocketjs/vm_clock.h"

#ifdef ESP_PLATFORM
#include "sdkconfig.h"

#ifdef CONFIG_POCKET_VM_CCOUNT
#include "esp_cpu.h"
/* CCOUNT is a PER-CORE register: two reads taken on different cores describe
 * two different counters and their difference is noise. A build that reads it
 * for the budget therefore needs the task that owns the drain to be unable to
 * migrate. That task is the ui task (docs/vm-L1-design.md sec.4.1) and
 * CONFIG_POCKET_UI_TASK_CORE is what pins it, so the two options are not
 * independent -- refuse the combination at compile time rather than ship a
 * budget that is wrong once in a few thousand frames (measured (device):
 * 1 migration in 1,800 idle frames, docs/vm-l1-clock.md sec.2). */
#if CONFIG_POCKET_UI_TASK_CORE < 0
#error "CONFIG_POCKET_VM_CCOUNT needs CONFIG_POCKET_UI_TASK_CORE >= 0: CCOUNT is per core, so a task that migrates between two reads makes their difference meaningless."
#endif
/* A constant rather than a runtime query of the clock tree, because
 * CONFIG_PM_ENABLE is off in this project: the CPU frequency never changes at
 * runtime, so asking would return this number every time. If dynamic frequency
 * scaling is ever turned on, THIS is the line that becomes wrong -- CCOUNT
 * counts CPU cycles, so its rate would follow the frequency. */
#define VM_CLOCK_TICKS_PER_US ((uint32_t)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ)
static vm_tick_t vm_clock_default(void) {
  return (vm_tick_t)esp_cpu_get_cycle_count();
}
#else
#include "esp_timer.h"
/* Truncated to 32 bits deliberately: only differences are used, and an
 * unsigned 32-bit difference of a microsecond counter is exact for any
 * interval shorter than 4,295 s. */
#define VM_CLOCK_TICKS_PER_US 1U
static vm_tick_t vm_clock_default(void) {
  return (vm_tick_t)esp_timer_get_time();
}
#endif

#else /* host */
#include <time.h>
#define VM_CLOCK_TICKS_PER_US 1U
static vm_tick_t vm_clock_default(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (vm_tick_t)((uint64_t)ts.tv_sec * 1000000u +
                     (uint64_t)ts.tv_nsec / 1000u);
}
#endif

/* File scope, not per guest: one guest runs at a time (spec sec.3 rule 1) and
 * a per-guest field would change the struct between test and shipping builds
 * for a value that is the same in both. */
static vm_clock_fn installed;
static uint32_t installed_ticks_per_us;

void vm_clock_install(vm_clock_fn fn, uint32_t ticks_per_us) {
  installed = fn;
  installed_ticks_per_us = ticks_per_us != 0U ? ticks_per_us : 1U;
}

vm_tick_t vm_clock_now(void) {
  return installed ? installed() : vm_clock_default();
}

uint32_t vm_clock_ticks_per_us(void) {
  return installed ? installed_ticks_per_us : VM_CLOCK_TICKS_PER_US;
}
