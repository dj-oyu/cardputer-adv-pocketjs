#include "sys_device.h"
#include "board.h"
#include "esp_timer.h"
#include <stddef.h>
static sys_state state;
sys_state *sys_device_state(void){return &state;}
static sys_power_state read_power(void *ctx,uint64_t now){
    (void)ctx;board_battery_t battery;
    bool valid=board_battery_read(&battery);
    return (sys_power_state){.sampled_at=valid?(uint64_t)battery.time_us:now,
        .millivolts=valid?battery.millivolts:0,.error=valid?0:1,.valid=valid,.sampled=true};
}
void sys_device_step(void){
    if(sys_power_deadline(&state)!=SYS_NEVER)
        sys_power_step(&state,(uint64_t)esp_timer_get_time(),read_power,NULL);
}
