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

// Call from the clock/SNTP service AFTER the system clock is set. Task-safe.
// A network disconnect does not invalidate an already-running clock. Set false
// only when the clock itself becomes untrustworthy; starts false.
void solar_time_set_synchronized(bool synchronized);

// Reads gettimeofday and believes it when it lands inside the supported window,
// whether or not this run was told about a sync: the chip powers up at the Unix
// epoch, so a modern wall clock was set by something, and the RTC carries it
// across a reset that the in-RAM flag does not survive. Does not start Wi-Fi,
// set system time, persist timestamps, or depend on the local timezone. A clock
// that fails or leaves the window after a sync returns demo days with
// UNAVAILABLE or OUT_OF_RANGE; before one it is simply DEMO.
solar_time_sample_t solar_time_now(double demo_elapsed_seconds);
