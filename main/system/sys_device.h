#ifndef SYS_DEVICE_H
#define SYS_DEVICE_H
#include "sys_state.h"
#include "sys_notify.h"
#include "sys_timer.h"
/* Firmware owner adapter. State survives guest attach/detach. */
sys_state *sys_device_state(void);
sys_notify *sys_device_notifications(void);
sys_timer *sys_device_timers(void);
void sys_device_step(void);
/* Owner-only, non-consuming. Zero means immediate work, SYS_NEVER means none.
 * Subscriber dirty bits are not work: only their own poll consumes them. */
uint64_t sys_device_next_deadline(void);
/* Bound an existing wait by the system deadline. Round UP to whole ticks;
 * the caller rechecks time/state after waking. No new periodic wake source. */
uint32_t sys_device_wait_ticks(uint64_t now_us,uint32_t max_ticks,uint32_t tick_hz);
bool sys_device_clock_read(sys_clock_state *out);
#endif
