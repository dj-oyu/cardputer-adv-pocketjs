#include <assert.h>
#include <stdio.h>
#include <sys/time.h>
#include "board.h"
static int64_t mono;
static struct timeval wall={.tv_sec=1800000000};
static unsigned wall_reads,adc_reads;
static int failure;
int64_t esp_timer_get_time(void){return mono;}
bool board_battery_read(board_battery_t *out){(void)out;adc_reads++;return false;}
static int fake_gettimeofday(struct timeval *out,void *zone){
    (void)zone;wall_reads++;*out=wall;return failure;
}
#define gettimeofday fake_gettimeofday
#include "../main/system/sys_clock.c"
#include "../main/system/sys_state.c"
#include "../main/system/sys_notify.c"
#include "../main/system/sys_device.c"
int main(void){
    sys_clock_state out;uint32_t dirty;sys_sub sub;
    assert(sys_subscribe(&state,SYS_CLOCK_CONFIG,&sub)==SYS_OK);
    sys_device_step();assert(wall_reads==1&&!adc_reads);
    assert(sys_clock_snapshot(&state,0,&out)&&out.source==SYS_CLOCK_RTC);
    assert(sys_poll(&state,sub,&dirty)==SYS_OK&&dirty==SYS_CLOCK_CONFIG);
    for(mono=0;mono<60000000;mono+=1000)sys_device_step();
    assert(wall_reads==1&&!adc_reads);
    assert(sys_poll(&state,sub,&dirty)==SYS_OK&&!dirty);
    assert(sys_clock_snapshot(&state,mono,&out)&&out.seconds==1800000060);
    wall.tv_sec-=3600;
    for(int i=0;i<100;i++)sys_clock_set_synchronized(true);
    sys_device_step();assert(wall_reads==2);
    assert(sys_clock_snapshot(&state,mono,&out)&&out.seconds==wall.tv_sec&&out.source==SYS_CLOCK_SNTP);
    failure=-1;sys_clock_set_synchronized(true);mono+=1000000;sys_device_step();
    assert(sys_clock_snapshot(&state,mono,&out)&&out.seconds==wall.tv_sec+1);
    assert(sys_clock_offer_pc(&state,1900000000,mono)==SYS_OK);
    sys_clock_set_synchronized(false);sys_device_step();
    assert(sys_clock_snapshot(&state,mono,&out)&&out.source==SYS_CLOCK_PC&&!out.trusted);
    failure=0;wall.tv_sec=0;sys_clock_set_synchronized(true);sys_device_step();
    assert(sys_clock_snapshot(&state,mono,&out)&&out.source==SYS_CLOCK_PC);
    assert(sys_poll(&state,sub,&dirty)==SYS_OK&&dirty==SYS_CLOCK_CONFIG);
    puts("SYSTEM_CLOCK_DEVICE_OK boot, no periodic wall/ADC IO, coalesced SNTP, holdover, PC fallback");
    sys_sub a,b;uint32_t id;
    assert(sys_subscribe(&state,SYS_NOTIFY,&a)==SYS_OK&&sys_subscribe(&state,SYS_NOTIFY,&b)==SYS_OK);
    assert(sys_poll(&state,a,&dirty)==SYS_OK&&dirty==SYS_NOTIFY);
    assert(sys_poll(&state,b,&dirty)==SYS_OK&&dirty==SYS_NOTIFY);
    assert(sys_notify_post(&notifications,1,0,"NOTICE",0,&id)==NOTICE_OK);
    sys_device_step();
    assert(sys_poll(&state,a,&dirty)==SYS_OK&&dirty==SYS_NOTIFY);
    assert(sys_poll(&state,a,&dirty)==SYS_OK&&!dirty);
    assert(sys_poll(&state,b,&dirty)==SYS_OK&&dirty==SYS_NOTIFY);
    sys_device_step();assert(sys_poll(&state,b,&dirty)==SYS_OK&&!dirty);
}
