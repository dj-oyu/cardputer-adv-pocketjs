#include <assert.h>
#include <stdio.h>
#include "../main/system/sys_state.c"
static unsigned reads;
static sys_power_state sample={.valid=true,.millivolts=4000};
static sys_power_state read_power(void *ctx,uint64_t now){
    (void)ctx;reads++;sample.sampled_at=now;return sample;
}
static uint32_t poll(sys_state *s,sys_sub sub){uint32_t mask=99;assert(sys_poll(s,sub,&mask)==SYS_OK);return mask;}
int main(void){
    sys_state s={0};sys_sub a,b;sys_power_state snapshot;
    assert(!sys_power_read(&s,&snapshot));
    for(uint64_t t=0;t<60000000;t+=33333)sys_power_step(&s,t,read_power,NULL);
    assert(!reads&&sys_power_deadline(&s)==SYS_NEVER);
    assert(sys_subscribe(&s,SYS_POWER,&a)==SYS_OK&&sys_subscribe(&s,SYS_POWER,&b)==SYS_OK);
    assert(poll(&s,a)==SYS_POWER&&poll(&s,a)==0&&poll(&s,b)==SYS_POWER);
    sys_power_step(&s,0,read_power,NULL);assert(reads==1);
    assert(poll(&s,a)==SYS_POWER&&poll(&s,b)==SYS_POWER);
    for(int i=0;i<100;i++)sys_power_refresh(&s);
    sys_power_step(&s,999999,read_power,NULL);assert(reads==1);
    sample.millivolts=4010;sys_power_step(&s,1000000,read_power,NULL);
    assert(reads==2&&poll(&s,a)==0&&poll(&s,b)==0);
    sample.millivolts=4019;sys_power_step(&s,2000000,read_power,NULL);assert(poll(&s,a)==0);
    sample.millivolts=4020;sys_power_step(&s,3000000,read_power,NULL);
    assert(poll(&s,a)==SYS_POWER&&poll(&s,b)==SYS_POWER);
    assert(sys_power_read(&s,&snapshot)&&snapshot.sampled_at==3000000&&snapshot.millivolts==4020);
    sample.valid=false;sample.error=1;sys_power_step(&s,4000000,read_power,NULL);
    assert(poll(&s,a)==SYS_POWER);sample.error=2;sys_power_step(&s,5000000,read_power,NULL);
    assert(poll(&s,a)==SYS_POWER&&poll(&s,b)==SYS_POWER);
    sys_power_step(&s,6000000,read_power,NULL);assert(poll(&s,a)==0);
    assert(sys_set_interest(&s,a,0)==SYS_OK&&sys_set_interest(&s,a,SYS_POWER)==SYS_OK);
    assert(poll(&s,a)==SYS_POWER&&poll(&s,b)==0);
    assert(sys_unsubscribe(&s,a)==SYS_OK&&sys_unsubscribe(&s,b)==SYS_OK);
    assert(sys_power_deadline(&s)==SYS_NEVER);
    uint32_t untouched=7;assert(sys_poll(&s,a,&untouched)==SYS_STALE&&untouched==7);
    sys_sub slots[8];for(int i=0;i<8;i++)assert(sys_subscribe(&s,SYS_POWER,&slots[i])==SYS_OK);
    assert(sys_subscribe(&s,SYS_POWER,&a)==SYS_FULL);
    for(int i=0;i<8;i++)assert(sys_unsubscribe(&s,slots[i])==SYS_OK);
    sys_power_refresh(&s);unsigned before=reads;
    sys_power_step(&s,7000000,read_power,NULL);assert(reads==before+1&&sys_power_deadline(&s)==SYS_NEVER);
    last_subscription=UINT32_MAX-1;
    assert(sys_subscribe(&s,SYS_POWER,&a)==SYS_OK&&a.value==UINT32_MAX);
    assert(sys_unsubscribe(&s,a)==SYS_OK&&sys_subscribe(&s,SYS_POWER,&b)==SYS_FULL);
    printf("SYSTEM_STATE PASS bytes=%zu subscriptions=%zu\n",sizeof(s),sizeof(s.subscriptions));
}
