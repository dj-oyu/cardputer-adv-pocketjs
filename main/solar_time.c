#include "solar_time.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <sys/time.h>

static atomic_bool synchronized;
void solar_time_set_synchronized(bool value) {
    atomic_store_explicit(&synchronized,value,memory_order_release);
}

solar_time_sample_t solar_time_now(double demo_elapsed_seconds) {
    if(!isfinite(demo_elapsed_seconds)||demo_elapsed_seconds<0)demo_elapsed_seconds=0;
    solar_time_sample_t sample={
        .days=fmin(demo_elapsed_seconds*.25,40*365.25),.source=SOLAR_TIME_DEMO};
    if(!atomic_load_explicit(&synchronized,memory_order_acquire))return sample;
    struct timeval now;
    if(gettimeofday(&now,NULL)!=0||now.tv_usec<0||now.tv_usec>=1000000) {
        sample.source=SOLAR_TIME_UNAVAILABLE;return sample;
    }
    int64_t seconds=(int64_t)now.tv_sec;
    // Intentional modern-clock subset of JPL's 1800--2050 fit: reject an
    // uninitialized Unix epoch, and never silently clamp an unsupported date.
    if(seconds<INT64_C(946684800)||seconds>=INT64_C(2524608000)) {
        sample.source=SOLAR_TIME_OUT_OF_RANGE;return sample;
    }
    // JD(UTC) = Unix/86400 + 2440587.5; J2000 = JD 2451545.0.
    // UTC is a display-level approximation to TDB here. TT-UTC/leap seconds
    // and the millisecond TT-TDB term belong in a future precision ephemeris
    // provider, not a timezone correction or a hardcoded future leap offset.
    sample.days=((double)(seconds-INT64_C(946728000))+now.tv_usec*1e-6)/86400.0;
    sample.source=SOLAR_TIME_UTC;
    return sample;
}
