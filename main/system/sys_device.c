#include "sys_device.h"
#include "sys_clock.h"
#include "board.h"
#include "esp_timer.h"
#include <stddef.h>
static sys_state state;
static sys_notify notifications;
static sys_timer timers;
sys_timer *sys_device_timers(void){return &timers;}
sys_notify *sys_device_notifications(void){return &notifications;}
sys_state *sys_device_state(void){return &state;}
uint64_t sys_device_next_deadline(void){
    if(sys_clock_update_pending()||notifications.changed||timers.changed)return 0;
    uint64_t next=sys_notify_deadline(&notifications),timer=sys_timer_deadline(&timers);
    uint64_t power=sys_power_deadline(&state);
    if(timer<next)next=timer;
    return power<next?power:next;
}
uint32_t sys_device_wait_ticks(uint64_t now,uint32_t cap,uint32_t hz){
    uint64_t next=sys_device_next_deadline();
    if(next==SYS_NEVER)return cap;
    if(next<=now||!hz)return 0;
    uint64_t delta=next-now,seconds=delta/1000000;
    /* Saturate before multiplying even for a caller near UINT64_MAX. */
    if(seconds>cap/hz)return cap;
    uint64_t ticks=seconds*hz+((delta%1000000)*hz+999999)/1000000;
    return ticks<cap?(uint32_t)ticks:cap;
}
bool sys_device_clock_read(sys_clock_state *out){
    return sys_clock_snapshot(&state,(uint64_t)esp_timer_get_time(),out);
}
static sys_power_state read_power(void *ctx,uint64_t now){
    (void)ctx;board_battery_t battery;
    bool valid=board_battery_read(&battery);
    return (sys_power_state){.sampled_at=valid?(uint64_t)battery.time_us:now,
        .millivolts=valid?battery.millivolts:0,.error=valid?0:1,.valid=valid,.sampled=true};
}
void sys_device_step(void){
    if(sys_notify_deadline(&notifications)!=UINT64_MAX||sys_timer_deadline(&timers)!=UINT64_MAX||notifications.changed){
        uint64_t now=(uint64_t)esp_timer_get_time();
        sys_notify_step(&notifications,now);
        bool notice_changed=sys_notify_take_changed(&notifications);
        sys_timer_step(&timers,&notifications,now,notice_changed);
        sys_notify_step(&notifications,now);
        if(sys_notify_take_changed(&notifications))notice_changed=true;
        if(notice_changed)sys_notify_publish(&state);
    }
    if(sys_timer_take_changed(&timers))sys_timer_publish(&state);
    if(sys_clock_take_update()){
        sys_clock_sample wall=sys_clock_read();
        uint64_t now=(uint64_t)esp_timer_get_time();
        if(wall.available&&wall.trusted&&wall.seconds>=INT64_C(946684800))
            sys_clock_update(&state,(sys_clock_anchor){.seconds=wall.seconds,
                .microseconds=wall.microseconds,.mono_us=now,
                .source=wall.synchronized?SYS_CLOCK_SNTP:SYS_CLOCK_RTC});
        else if(!wall.trusted)
            sys_clock_update(&state,(sys_clock_anchor){.mono_us=now});
        else if(wall.available)
            sys_clock_fail(&state,now,SYS_CLOCK_OUT_OF_RANGE);
        else if(!state.clock.source)
            sys_clock_fail(&state,now,SYS_CLOCK_UNAVAILABLE);
        /* A read error after synchronization does not discard good holdover. */
    }
    if(sys_power_deadline(&state)!=SYS_NEVER)
        sys_power_step(&state,(uint64_t)esp_timer_get_time(),read_power,NULL);
}
