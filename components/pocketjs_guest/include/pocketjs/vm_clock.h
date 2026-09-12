#pragma once

/* VM L1 (docs/vm-L1-design.md sec.1.3): the one place the scheduler learns
 * what time it is. Everything else takes DIFFERENCES of whatever this returns,
 * so the unit only has to be a monotonic tick -- which counter supplies the
 * ticks, and how many of them make a microsecond, is a build decision (CCOUNT
 * on the device, CLOCK_MONOTONIC on the host, a constant 0 in the
 * deterministic count mode the corpus runs in).
 *
 * Deliberately free of esp_err.h and quickjs.h: tools/vmtest links this file
 * rather than copying it, so the host and the firmware can never drift. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** One tick of the counter vm_clock_now() reads. 32 bits ON PURPOSE.
 *
 * The cheapest clock this chip has is CCOUNT -- 6 cycles = 25 ns a read
 * against esp_timer_get_time()'s 200 cycles = 833 ns, measured (device),
 * docs/vm-l1-clock.md sec.1 -- and CCOUNT is a 32-bit cycle counter that wraps
 * every 2^32/240e6 ~= 17.9 s at 240 MHz. An UNSIGNED 32-bit subtraction is
 * still exactly right across that wrap (C's mod-2^32 arithmetic returns the
 * true difference for any interval shorter than the period), and a turn budget
 * is 8 ms -- three orders of magnitude short of it. So the wrap is handled by
 * the type rather than by a branch, and widening the type would buy nothing
 * while costing a second read. */
typedef uint32_t vm_tick_t;
typedef vm_tick_t (*vm_clock_fn)(void);

/** The tick counter. Device: CCOUNT when CONFIG_POCKET_VM_CCOUNT is set (which
 * requires a pinned ui task, because CCOUNT is per core), otherwise
 * esp_timer_get_time() truncated to 32 bits -- whose difference is exact the
 * same way for any interval under 2^32 us = 4,295 s. Host: CLOCK_MONOTONIC
 * microseconds, same truncation. */
vm_tick_t vm_clock_now(void);

/** Ticks per microsecond of that counter: 240 for CCOUNT at 240 MHz, 1 for a
 * microsecond timer. THE BUDGET IS STATED IN MICROSECONDS EVERYWHERE OUTSIDE
 * vm_sched.c (VM_TURN_BUDGET_US, VM_RUNAWAY_US, the RUNAWAY log line), and
 * this is the only place the two units meet: the conversion happens once when
 * a budget is armed (us -> ticks, one multiply) and once when a drain returns
 * (ticks -> us, one divide). Never per job -- the per-job check is a subtract
 * and a compare in ticks. */
uint32_t vm_clock_ticks_per_us(void);

/** Swap the source. A NULL fn restores the platform default and ignores
 * `ticks_per_us`. Tests install a clock that always returns 0 to make the
 * budget purely count-driven. */
void vm_clock_install(vm_clock_fn fn, uint32_t ticks_per_us);

#ifdef __cplusplus
}
#endif
