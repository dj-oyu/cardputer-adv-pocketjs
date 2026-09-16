#include <assert.h>
#include <stdio.h>
#include "../main/system/sys_state.c"
static uint32_t poll(sys_state *s,sys_sub sub){
    uint32_t mask;assert(sys_poll(s,sub,&mask)==SYS_OK);return mask;
}
int main(void){
    sys_state s={0};sys_sub a,b,power;sys_clock_state out;
    assert(sys_subscribe(&s,SYS_CLOCK_CONFIG,&a)==SYS_OK);
    assert(sys_subscribe(&s,SYS_CLOCK_CONFIG,&b)==SYS_OK);
    assert(sys_subscribe(&s,SYS_POWER,&power)==SYS_OK);
    assert(poll(&s,a)==SYS_CLOCK_CONFIG&&poll(&s,b)==SYS_CLOCK_CONFIG);
    assert(poll(&s,power)==SYS_POWER);
    assert(!sys_clock_snapshot(&s,0,&out)&&out.revision==0);
    assert(sys_clock_offer_pc(&s,1,0)==SYS_INVALID);
    assert(sys_clock_offer_pc(&s,1700000000,100)==SYS_OK);
    assert(poll(&s,a)==SYS_CLOCK_CONFIG&&poll(&s,b)==SYS_CLOCK_CONFIG&&poll(&s,power)==0);
    assert(sys_clock_snapshot(&s,1500100,&out)&&out.seconds==1700000001&&out.microseconds==500000);
    assert(out.source==SYS_CLOCK_PC&&!out.trusted);
    uint32_t revision=out.revision;
    for(uint64_t now=100;now<60000000;now+=33333)assert(sys_clock_snapshot(&s,now,&out));
    assert(out.revision==revision&&poll(&s,a)==0&&poll(&s,b)==0);
    assert(sys_clock_offer_pc(&s,1700000001,1000100)==SYS_OK&&poll(&s,a)==0);
    sys_clock_anchor rtc={.seconds=1800000000,.microseconds=999999,.mono_us=2000000,.source=SYS_CLOCK_RTC};
    assert(sys_clock_update(&s,rtc)==SYS_OK&&poll(&s,a)==SYS_CLOCK_CONFIG);
    assert(sys_clock_snapshot(&s,2000001,&out)&&out.seconds==1800000001&&out.microseconds==0&&out.trusted);
    assert(sys_clock_offer_pc(&s,1900000000,3000000)==SYS_OK&&poll(&s,a)==0);
    assert(sys_clock_snapshot(&s,3000000,&out)&&out.source==SYS_CLOCK_RTC);
    rtc.source=SYS_CLOCK_SNTP;
    assert(sys_clock_update(&s,rtc)==SYS_OK&&poll(&s,a)==SYS_CLOCK_CONFIG);
    assert(sys_clock_update(&s,rtc)==SYS_OK&&poll(&s,a)==0);
    rtc.seconds-=3600;
    assert(sys_clock_update(&s,rtc)==SYS_OK&&poll(&s,a)==SYS_CLOCK_CONFIG);
    assert(sys_clock_update(&s,(sys_clock_anchor){.mono_us=4000000})==SYS_OK);
    assert(sys_clock_snapshot(&s,4000000,&out)&&out.seconds==1900000001&&out.source==SYS_CLOCK_PC);
    assert(poll(&s,a)==SYS_CLOCK_CONFIG&&poll(&s,b)==SYS_CLOCK_CONFIG);
    assert(sys_clock_timezone(&s,32400)==SYS_OK&&poll(&s,a)==SYS_CLOCK_CONFIG);
    assert(sys_clock_timezone(&s,32400)==SYS_OK&&poll(&s,a)==0);
    assert(sys_clock_timezone(&s,50401)==SYS_INVALID);
    assert(sys_clock_snapshot(&s,4000000,&out)&&out.utc_offset==32400&&out.seconds==1900000001);
    assert(!sys_clock_snapshot(&s,2999999,&out));
    rtc.seconds=INT64_MAX;rtc.microseconds=999999;
    assert(sys_clock_update(&s,rtc)==SYS_OK);
    assert(!sys_clock_snapshot(&s,rtc.mono_us+1,&out));
    rtc.seconds=-1;assert(sys_clock_update(&s,rtc)==SYS_INVALID);
    rtc.seconds=1800000000;rtc.microseconds=1000000;assert(sys_clock_update(&s,rtc)==SYS_INVALID);
    assert(sys_set_interest(&s,power,SYS_POWER|SYS_CLOCK_CONFIG)==SYS_OK);
    assert(poll(&s,power)==SYS_CLOCK_CONFIG);
    printf("SYSTEM_CLOCK_STATE_OK anchor, independent dirty, coalescing, precedence, timezone, bounds bytes=%zu\n",sizeof(s));
}
