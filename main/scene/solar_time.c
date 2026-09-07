#include "solar_time.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <sys/time.h>

// Three states, not two. UNKNOWN is where a boot starts: nothing has claimed
// the clock either way, so the clock is asked. TRUSTED and DISTRUSTED are what
// a caller asserts, and DISTRUSTED has to outrank the clock's own evidence --
// otherwise "this clock is wrong" would be a statement the code ignores.
//
// The reason UNKNOWN consults the clock at all: the in-RAM flag does not
// survive a reset and the RTC does. The chip powers up at the Unix epoch, which
// the range test below rejects, so a wall clock inside the supported window was
// set by something -- and the only thing here that sets it is the SNTP sync.
// Testing rather than remembering is what stops a reboot from discarding a
// clock that is still right, which is what it used to do: sync, reboot, and the
// sail scene silently went back to demo time with a good clock in the RTC.
enum { TRUST_UNKNOWN=0, TRUST_YES=1, TRUST_NO=-1 };
static atomic_int trust;

void solar_time_set_synchronized(bool value) {
    atomic_store_explicit(&trust,value?TRUST_YES:TRUST_NO,memory_order_release);
}

solar_time_sample_t solar_time_now(double demo_elapsed_seconds) {
    if(!isfinite(demo_elapsed_seconds)||demo_elapsed_seconds<0)demo_elapsed_seconds=0;
    solar_time_sample_t sample={
        .days=fmin(demo_elapsed_seconds*.25,40*365.25),.source=SOLAR_TIME_DEMO};
    int state=atomic_load_explicit(&trust,memory_order_acquire);
    if(state==TRUST_NO)return sample;
    struct timeval now;
    if(gettimeofday(&now,NULL)!=0||now.tv_usec<0||now.tv_usec>=1000000) {
        // A failure is only news once something claimed the clock was good.
        // Before that it is the ordinary state of a device that never synced.
        if(state==TRUST_YES)sample.source=SOLAR_TIME_UNAVAILABLE;
        return sample;
    }
    int64_t seconds=(int64_t)now.tv_sec;
    // Intentional modern-clock subset of JPL's 1800--2050 fit: reject an
    // uninitialized Unix epoch, and never silently clamp an unsupported date.
    if(seconds<INT64_C(946684800)||seconds>=INT64_C(2524608000)) {
        if(state==TRUST_YES)sample.source=SOLAR_TIME_OUT_OF_RANGE;
        return sample;
    }
    // JD(UTC) = Unix/86400 + 2440587.5; J2000 = JD 2451545.0.
    // UTC is a display-level approximation to TDB here. TT-UTC/leap seconds
    // and the millisecond TT-TDB term belong in a future precision ephemeris
    // provider, not a timezone correction or a hardcoded future leap offset.
    sample.days=((double)(seconds-INT64_C(946728000))+now.tv_usec*1e-6)/86400.0;
    sample.source=SOLAR_TIME_UTC;
    if(state==TRUST_UNKNOWN)
        atomic_store_explicit(&trust,TRUST_YES,memory_order_release);
    return sample;
}
