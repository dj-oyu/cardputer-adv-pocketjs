#include "pocketjs/vm_clock.h"

#ifdef ESP_PLATFORM
#include "esp_timer.h"
static int64_t vm_clock_default(void) { return esp_timer_get_time(); }
#else
#include <time.h>
static int64_t vm_clock_default(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
#endif

/* File scope, not per guest: one guest runs at a time (spec sec.3 rule 1) and
 * a per-guest field would change the struct between test and shipping builds
 * for a value that is the same in both. */
static vm_clock_fn installed;

void vm_clock_install(vm_clock_fn fn) { installed = fn; }

int64_t vm_clock_now_us(void) {
  return installed ? installed() : vm_clock_default();
}
