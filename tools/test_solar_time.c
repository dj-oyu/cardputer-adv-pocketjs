// gcc -O2 -Wall -Wextra -Werror tools/test_solar_time.c -I tools -I tools/hostshim -lm -o /tmp/test-solar-time
//
// The clock is the real anchor core driven from the host (hostshim/solar_clock.h
// includes main/system/sys_clock.c and the state it reads), so the times below go
// in through sys_clock_update() and nothing here depends on the host's own
// gettimeofday -- the 2038 case used to be at the mercy of a 4-byte tv_sec under
// MinGW, which is what the WSL-only warning on this file was about.
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../main/scene/fxmath.c"
// The fixed-point trig the scene files call (main/scene/fxmath.c): the scene
// .c files below are included whole, so the definition has to be in this TU too.
#include "hostshim/solar_clock.h"
#include "../main/scene/scene_mem.c"
#include "../main/scene/solar_time.c"
#include "../main/scene/solar_sail.c"
static void set_clock(int64_t seconds,int32_t micros){
    assert(sys_clock_update(&solar_clock,(sys_clock_anchor){.seconds=seconds,
        .microseconds=micros,.mono_us=solar_mono,.source=SYS_CLOCK_RTC})==SYS_OK);
}
int main(void){
    assert(solar_time_now(100).source==SOLAR_TIME_DEMO&&solar_time_now(100).days==25);
    assert(solar_time_now(-1).days==0&&solar_time_now(NAN).days==0&&solar_time_now(1e12).days==14610);
    set_clock(946728000,0);assert(solar_time_now(100).days==0);
    solar_mono=500000;assert(fabs(solar_time_now(0).days-.5/86400)<1e-12);
    set_clock(951782400,0);assert(solar_time_now(0).days==58.5);
    set_clock(951868800,0);assert(solar_time_now(0).days==59.5);
    set_clock(INT64_C(2147483648),0);assert(solar_time_now(0).source==SOLAR_TIME_UTC);
    set_clock(INT64_C(2524607999),0);assert(solar_time_now(0).source==SOLAR_TIME_UTC);
    set_clock(INT64_C(2524608000),0);assert(solar_time_now(4).source==SOLAR_TIME_OUT_OF_RANGE);
    assert(sys_clock_fail(&solar_clock,solar_mono,SYS_CLOCK_UNAVAILABLE)==SYS_OK);
    assert(solar_time_now(4).source==SOLAR_TIME_UNAVAILABLE);
    assert(sys_clock_fail(&solar_clock,solar_mono,SYS_CLOCK_OUT_OF_RANGE)==SYS_OK);
    assert(solar_time_now(4).source==SOLAR_TIME_OUT_OF_RANGE);
    set_clock(1788739200,0);elapsed=84;solar_sail_prepare(0,0,0);
    assert(focus==2&&strcmp(solar_sail_time_label(),"UTC")==0);
    double before=sim_days;solar_mono+=1000000;solar_sail_prepare(.033f,0,0);
    assert(fabs((sim_days-before)*86400-1)<1e-6);
    solar_mono+=UINT64_C(3600000000);solar_sail_prepare(.033f,0,0);
    assert(focus==2&&elapsed<85&&fabs((sim_days-before)*86400-3601)<1e-6);
    set_clock(1788739200+1801,0);solar_sail_prepare(.033f,0,0);
    assert(focus==2&&fabs((sim_days-before)*86400-1801)<1e-6);
    assert(sys_clock_update(&solar_clock,(sys_clock_anchor){.mono_us=solar_mono})==SYS_OK);
    assert(solar_time_now(4).source==SOLAR_TIME_DEMO);
    assert(sys_clock_offer_pc(&solar_clock,1788739200,solar_mono)==SYS_OK);
    assert(solar_time_now(4).source==SOLAR_TIME_UTC);
    solar_time_set_synchronized(false);assert(sys_clock_take_update());
    puts("SOLAR_TIME_OK shared anchor, PC fallback, J2000, leap day, 2038, range, errors, holdover, resync, tour independence");
}
