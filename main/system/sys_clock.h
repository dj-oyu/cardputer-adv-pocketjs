#ifndef SYS_CLOCK_H
#define SYS_CLOCK_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int64_t seconds;
    int32_t microseconds;
    bool available, trusted;
} sys_clock_sample;

/* Compatibility wall-clock provider. No allocation, timezone conversion or
 * application date range. An unset RTC is not evidence of synchronization. */
sys_clock_sample sys_clock_read(void);
/* Task-safe assertion after setting the platform clock. A failed sync must
 * not revoke an otherwise running clock. False explicitly revokes trust. */
void sys_clock_set_synchronized(bool synchronized);
#endif
