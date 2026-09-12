#pragma once

/* VM L1 (docs/vm-L1-design.md sec.1.3): the one place the scheduler learns
 * what time it is. Everything else takes differences of whatever this returns,
 * so the unit only has to be monotonic microseconds -- which clock supplies
 * them is a build decision (esp_timer on the device, CLOCK_MONOTONIC on the
 * host, a constant 0 in the deterministic count mode the corpus runs in).
 *
 * Deliberately free of esp_err.h and quickjs.h: tools/vmtest links this file
 * rather than copying it, so the host and the firmware can never drift. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Monotonic microseconds. Only differences are meaningful; a source that
 * wraps (a 32-bit cycle counter) is fine as long as a turn is far shorter
 * than its period. */
typedef int64_t (*vm_clock_fn)(void);

/** The platform default: esp_timer_get_time() on the device (the same clock
 * app_session.c's 250 ms deadline uses, so a turn start can be one read),
 * clock_gettime(CLOCK_MONOTONIC) on the host. */
int64_t vm_clock_now_us(void);

/** Swap the source. NULL restores the platform default. Tests install a clock
 * that always returns 0 to make the budget purely count-driven. */
void vm_clock_install(vm_clock_fn fn);

#ifdef __cplusplus
}
#endif
