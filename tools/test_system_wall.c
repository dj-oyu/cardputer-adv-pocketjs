#include <assert.h>
#include <stdio.h>
#include "../main/system/sys_state.c"
#include "../main/system/sys_notify.c"
#include "../main/system/sys_wall.c"
#include "../main/system/sys_ringer.c"
int main(void){
    sys_state clock={0};sys_notify notices={0};sys_wall wall={.owner=1,.invalid=true};
    uint32_t day=20000,last=0,reset=0,notified=0;int32_t minute=420,offset=0;
    uint32_t morning=day*86400+7*3600;
    wall.rules[0]=(sys_wall_rule){.last=&last,.minute=&minute,.offset=&offset,.label="DAILY"};
    wall.rules[1]=(sys_wall_rule){.due=&reset,.last=&notified,.label="USAGE"};
    assert(sys_clock_update(&clock,(sys_clock_anchor){.seconds=morning-1,.microseconds=500000,.source=SYS_CLOCK_RTC})==SYS_OK);
    sys_wall_step(&wall,&clock,&notices,0);
    assert(sys_wall_deadline(&wall,&clock,&notices)==500000&&!last);
    sys_wall_step(&wall,&clock,&notices,499999);assert(!last);
    sys_wall_step(&wall,&clock,&notices,500000);assert(last==day&&sys_wall_take_changed(&wall));
    sys_notify_step(&notices,500000);sys_notice active;assert(sys_notify_active(&notices,&active));
    sys_ringer ring={0};assert(sys_ringer_deadline(&ring,&notices)==0);
    assert(sys_ringer_poll(&ring,&notices,500000));
    assert(!sys_ringer_poll(&ring,&notices,2499999));
    assert(sys_ringer_poll(&ring,&notices,2500000));
    assert(sys_ringer_poll(&ring,&notices,29000000)); /* Late poll grants only one. */
    assert(!sys_ringer_poll(&ring,&notices,30500000));
    assert(sys_ringer_deadline(&ring,&notices)==UINT64_MAX);
    assert(sys_notify_ack(&notices,1,active.id)==NOTICE_OK);
    assert(!sys_ringer_poll(&ring,&notices,30500000));
    /* A clock step to a prior day and return cannot replay the daily alarm. */
    sys_clock_update(&clock,(sys_clock_anchor){.seconds=morning-86400,.source=SYS_CLOCK_RTC});
    sys_wall_step(&wall,&clock,&notices,0);assert(last==day&&!sys_wall_take_changed(&wall));
    sys_clock_update(&clock,(sys_clock_anchor){.seconds=morning,.source=SYS_CLOCK_RTC});
    sys_wall_step(&wall,&clock,&notices,0);assert(last==day&&!sys_wall_take_changed(&wall));
    /* Missed daily minutes are skipped, whereas usage resets catch up. */
    sys_clock_update(&clock,(sys_clock_anchor){.seconds=morning+86460,.source=SYS_CLOCK_RTC});
    reset=morning+86400;sys_wall_invalidate(&wall);sys_wall_step(&wall,&clock,&notices,0);
    assert(last==day&&notified==reset&&sys_wall_take_changed(&wall));
    sys_notify_release_owner(&notices,1);
    /* Queue pressure preserves watermark, and retries only on notification change. */
    uint32_t id;for(unsigned i=0;i<8;i++)assert(sys_notify_post(&notices,2,0,"FULL",0,&id)==NOTICE_OK);
    reset++;sys_wall_invalidate(&wall);sys_wall_step(&wall,&clock,&notices,0);
    assert(notified!=reset&&wall.blocked);
    uint64_t deadline=sys_wall_deadline(&wall,&clock,&notices);assert(deadline>0);
    sys_wall_step(&wall,&clock,&notices,1);assert(notified!=reset);
    sys_notify_release_owner(&notices,2);assert(sys_wall_deadline(&wall,&clock,&notices)==0);
    sys_wall_step(&wall,&clock,&notices,1);assert(notified==reset);
    /* A snoozed/replaced notice cancels the previous sound deadline. */
    sys_notify_step(&notices,1);assert(sys_notify_active(&notices,&active));
    assert(sys_ringer_poll(&ring,&notices,1));
    assert(sys_notify_snooze(&notices,1,active.id,100)==NOTICE_OK);
    assert(sys_ringer_deadline(&ring,&notices)==UINT64_MAX&&!sys_ringer_poll(&ring,&notices,2));
    sys_notify_step(&notices,100);assert(sys_ringer_poll(&ring,&notices,100));
    assert(sys_notify_snooze(&notices,1,active.id,200)==NOTICE_OK);
    sys_notify_step(&notices,200); /* No poll during the snoozed interval. */
    assert(sys_ringer_deadline(&ring,&notices)==0&&sys_ringer_poll(&ring,&notices,200));
    last=UINT32_MAX;reset=0;sys_wall_invalidate(&wall);
    sys_wall_step(&wall,&clock,&notices,100);
    assert(sys_wall_deadline(&wall,&clock,&notices)==SYS_NEVER);
    sys_notify_release_owner(&notices,1);
    for(unsigned i=0;i<8;i++)assert(sys_notify_post(&notices,2,0,"FULL",0,&id)==NOTICE_OK);
    last=0;sys_clock_update(&clock,(sys_clock_anchor){.seconds=morning,.source=SYS_CLOCK_RTC});
    sys_wall_invalidate(&wall);sys_wall_step(&wall,&clock,&notices,0);
    assert(!last&&sys_wall_deadline(&wall,&clock,&notices)==60000000);
    sys_wall_step(&wall,&clock,&notices,60000000);assert(!last);
    sys_notify_release_owner(&notices,2);sys_wall_step(&wall,&clock,&notices,60000000);
    assert(!last); /* Capacity arriving after the minute must not ring late. */
    puts("SYSTEM_WALL_OK fractional deadline, catchup, daily minute, backward clock, pressure, tone lifecycle");
}
