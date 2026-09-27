#include "pocketjs/vm_clock.h"

#ifdef ESP_PLATFORM
#include "sdkconfig.h"

#include "esp_timer.h"
/* Truncated to 32 bits deliberately: only differences are used, and an
 * unsigned 32-bit difference of a microsecond counter is exact for any
 * interval shorter than 4,295 s. */
#define VM_CLOCK_TICKS_PER_US 1U
static vm_tick_t vm_clock_default(void) {
  return (vm_tick_t)esp_timer_get_time();
}

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
