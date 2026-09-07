// cc -std=c11 -O2 -Wall -Wextra -fsanitize=address,undefined tools/test_solar_time.c -lm -o /tmp/test-solar-time
#include <sys/time.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
static struct timeval fake_now;
static int fake_error,reads;
static int fake_gettimeofday(struct timeval *out,void *zone) {
    (void)zone;reads++;
    if(fake_error)return -1;
    *out=fake_now;return 0;
}
#define gettimeofday fake_gettimeofday
#include "../main/solar_time.c"
#include "../main/solar_sail.c"

int main(void) {
    // A boot with nothing claimed asks the clock rather than assuming: the RTC
    // outlives the flag, so a reset must not throw away a synchronized clock.
    // An unset clock reads the epoch and is refused, which is DEMO, not a
    // failure -- a device that never synced has not gone wrong.
    solar_time_sample_t t=solar_time_now(100);
    assert(t.source==SOLAR_TIME_DEMO&&t.days==25&&reads==1);
    assert(solar_time_now(-1).days==0&&solar_time_now(NAN).days==0);
    assert(solar_time_now(1e12).days==14610);
    solar_time_set_synchronized(true);
    fake_now=(struct timeval){.tv_sec=946728000,.tv_usec=0};
    t=solar_time_now(100);assert(t.source==SOLAR_TIME_UTC&&t.days==0);
    fake_now.tv_usec=500000;assert(fabs(solar_time_now(0).days-.5/86400)<1e-12);
    fake_now=(struct timeval){.tv_sec=951782400}; // 2000-02-29 00:00 UTC.
    assert(solar_time_now(0).days==58.5);
    fake_now.tv_sec=951868800;assert(solar_time_now(0).days==59.5);
    fake_now.tv_sec=2147483648LL; // 2038 rollover must remain valid.
    assert(solar_time_now(0).source==SOLAR_TIME_UTC);
    fake_now.tv_sec=2524607999LL;assert(solar_time_now(0).source==SOLAR_TIME_UTC);
    fake_now.tv_sec=2524608000LL;assert(solar_time_now(4).source==SOLAR_TIME_OUT_OF_RANGE);
    fake_now.tv_sec=0;assert(solar_time_now(4).days==1);
    assert(solar_time_now(4).source==SOLAR_TIME_OUT_OF_RANGE);
    fake_error=1;assert(solar_time_now(4).source==SOLAR_TIME_UNAVAILABLE);fake_error=0;
    fake_now.tv_usec=1000000;assert(solar_time_now(0).source==SOLAR_TIME_UNAVAILABLE);
    fake_now=(struct timeval){.tv_sec=1788739200}; // 2026-09-07 UTC.
    elapsed=84;solar_sail_prepare(0,0,0);
    assert(focus==2&&strcmp(solar_sail_time_label(),"UTC")==0);
    double before=sim_days;
    fake_now.tv_sec+=1;solar_sail_prepare(.033f,0,0);
    assert(fabs((sim_days-before)*86400-1)<1e-6); // Not accelerated demo time.
    // Wall time advances during app use/offline holdover; tour does not skip.
    fake_now.tv_sec+=3600;solar_sail_prepare(.033f,0,0);
    assert(focus==2&&elapsed<85&&fabs((sim_days-before)*86400-3601)<1e-6);
    // Resynchronization, including backward correction, does not reset tour.
    fake_now.tv_sec-=1800;solar_time_set_synchronized(true);solar_sail_prepare(.033f,0,0);
    assert(focus==2&&fabs((sim_days-before)*86400-1801)<1e-6);
    // Distrust outranks the clock's own evidence, or "this clock is wrong"
    // would be a statement the code quietly ignores.
    solar_time_set_synchronized(false);solar_sail_prepare(0,0,0);
    assert(strcmp(solar_sail_time_label(),"DEMO")==0&&sim_days==elapsed*.25);
    // A reboot loses the flag, not the RTC. Rediscovering the clock is what
    // stops the sail scene from reverting to demo time after a restart.
    trust=TRUST_UNKNOWN;
    assert(solar_time_now(4).source==SOLAR_TIME_UTC);
    assert(atomic_load(&trust)==TRUST_YES);
    // Failures stay distinguishable from never having synced, both ways round.
    trust=TRUST_UNKNOWN;fake_error=1;
    assert(solar_time_now(4).source==SOLAR_TIME_DEMO);
    solar_time_set_synchronized(true);
    assert(solar_time_now(4).source==SOLAR_TIME_UNAVAILABLE);fake_error=0;
    trust=TRUST_UNKNOWN;fake_now.tv_sec=0;
    assert(solar_time_now(4).source==SOLAR_TIME_DEMO);
    solar_time_set_synchronized(true);
    assert(solar_time_now(4).source==SOLAR_TIME_OUT_OF_RANGE);
    puts("SOLAR_TIME_OK unsynced, UTC, leap day, range, 2038, errors, holdover, resync, reboot, distrust, tour independence");
}
