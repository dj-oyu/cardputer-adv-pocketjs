#include <assert.h>
#include <stdio.h>
#include <sys/time.h>
#include "../main/system/sys_clock.h"
static struct timeval wall;
static int failure, revoke, reads;
static int fake_gettimeofday(struct timeval *out,void *zone) {
    (void)zone;reads++;
    if(revoke)sys_clock_set_synchronized(false);
    *out=wall;return failure;
}
#define gettimeofday fake_gettimeofday
#include "../main/system/sys_clock.c"

int main(void) {
    sys_clock_sample s=sys_clock_read();
    assert(s.available&&!s.trusted&&s.seconds==0);
    wall=(struct timeval){.tv_sec=2524608000LL,.tv_usec=123456};
    s=sys_clock_read();
    assert(s.available&&s.trusted&&s.seconds==2524608000LL&&s.microseconds==123456);
    wall.tv_sec=INT64_C(4294967296);
    assert(sys_clock_read().seconds==INT64_C(4294967296));
    failure=-1;s=sys_clock_read();assert(!s.available&&s.trusted);
    failure=0;wall.tv_usec=-1;assert(!sys_clock_read().available);
    wall.tv_usec=1000000;assert(!sys_clock_read().available);
    wall.tv_usec=0;sys_clock_set_synchronized(false);
    int before=reads;s=sys_clock_read();assert(!s.available&&!s.trusted&&reads==before);
    sys_clock_set_synchronized(true);assert(sys_clock_read().trusted);
    trust=TRUST_UNKNOWN;revoke=1;
    s=sys_clock_read();assert(!s.available&&!s.trusted&&atomic_load(&trust)==TRUST_NO);
    puts("SYSTEM_CLOCK_OK RTC recovery, 2050/2106, precision, failure, distrust, concurrent revocation");
}
