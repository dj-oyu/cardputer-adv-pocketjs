#ifndef SYS_CLOCK_H
#define SYS_CLOCK_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int64_t seconds;
    int32_t microseconds;
    bool available, trusted, synchronized;
} sys_clock_sample;

/* Compatibility wall-clock provider. No allocation, timezone conversion or
 * application date range. An unset RTC is not evidence of synchronization. */
sys_clock_sample sys_clock_read(void);
/* Task-safe assertion after setting the platform clock. A failed sync must
 * not revoke an otherwise running clock. False explicitly revokes trust. */
void sys_clock_set_synchronized(bool synchronized);
/* Single owner consumes a coalesced boot/configuration request. No payload is
 * shared across tasks: the owner reads the platform clock after consumption. */
bool sys_clock_take_update(void);
bool sys_clock_update_pending(void);
#endif
