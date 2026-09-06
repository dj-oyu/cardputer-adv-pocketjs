#pragma once
#include <stdbool.h>

typedef enum {
    SOLAR_TIME_DEMO,
    SOLAR_TIME_UTC,
    SOLAR_TIME_UNAVAILABLE,
    SOLAR_TIME_OUT_OF_RANGE
} solar_time_source_t;
typedef struct {
    double days; // Days from J2000 noon; UTC approximates ephemeris time.
    solar_time_source_t source;
} solar_time_sample_t;

// Call from the future clock/SNTP service AFTER the system clock is set.
// Task-safe. A network disconnect does not invalidate an already-running clock.
// Set false only when the clock itself becomes untrustworthy; starts false.
void solar_time_set_synchronized(bool synchronized);

// Uses gettimeofday only after explicit synchronization. Does not start Wi-Fi,
// set system time, persist timestamps, or depend on the local timezone.
// Bad/unsupported clocks return demo days with a non-UTC source status.
solar_time_sample_t solar_time_now(double demo_elapsed_seconds);
