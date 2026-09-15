#include "sys_state.h"
#include <stddef.h>
/* Process-lifetime IDs fail closed at exhaustion, even across service reuse. */
static uint32_t last_subscription;
_Static_assert(sizeof(sys_subscription)<=16,"subscription budget");
_Static_assert(sizeof(sys_state)<=256,"system state budget");
static sys_subscription *find(sys_state *s,sys_sub id){
    if(s&&id.value)for(unsigned i=0;i<SYS_SUBSCRIPTIONS;i++)
        if(s->subscriptions[i].id==id.value)return &s->subscriptions[i];
    return NULL;
}
sys_result sys_subscribe(sys_state *s,uint32_t interest,sys_sub *out){
    if(!s||!out||(interest&~SYS_TOPICS))return SYS_INVALID;
    if(last_subscription==UINT32_MAX)return SYS_FULL;
    for(unsigned i=0;i<SYS_SUBSCRIPTIONS;i++)if(!s->subscriptions[i].id){
        s->subscriptions[i]=(sys_subscription){++last_subscription,interest,interest};
        s->power_users+=(interest&SYS_POWER)!=0;
        *out=(sys_sub){last_subscription};return SYS_OK;
    }
    return SYS_FULL;
}
sys_result sys_set_interest(sys_state *s,sys_sub id,uint32_t interest){
    if(interest&~SYS_TOPICS)return SYS_INVALID;
    sys_subscription *sub=find(s,id);if(!sub)return SYS_STALE;
    s->power_users-=(sub->interest&SYS_POWER)!=0;
    s->power_users+=(interest&SYS_POWER)!=0;
    sub->pending=(sub->pending|(interest&~sub->interest))&interest;
    sub->interest=interest;return SYS_OK;
}
sys_result sys_unsubscribe(sys_state *s,sys_sub id){
    sys_subscription *sub=find(s,id);if(!sub)return SYS_STALE;
    s->power_users-=(sub->interest&SYS_POWER)!=0;
    *sub=(sys_subscription){0};return SYS_OK;
}
sys_result sys_poll(sys_state *s,sys_sub id,uint32_t *changed){
    if(!changed)return SYS_INVALID;
    sys_subscription *sub=find(s,id);if(!sub)return SYS_STALE;
    *changed=sub->pending;sub->pending=0;return SYS_OK;
}
bool sys_power_read(const sys_state *s,sys_power_state *out){
    if(!s||!out)return false;
    *out=s->power;return out->sampled;
}
void sys_notify_publish(sys_state *s){
    if(s)for(unsigned i=0;i<SYS_SUBSCRIPTIONS;i++)
        s->subscriptions[i].pending|=s->subscriptions[i].interest&SYS_NOTIFY;
}
void sys_timer_publish(sys_state *s){
    if(s)for(unsigned i=0;i<SYS_SUBSCRIPTIONS;i++)
        s->subscriptions[i].pending|=s->subscriptions[i].interest&SYS_TIMER;
}
static void publish_clock(sys_state *s){
    if(s->clock_revision!=UINT32_MAX)s->clock_revision++;
    for(unsigned i=0;i<SYS_SUBSCRIPTIONS;i++)
        s->subscriptions[i].pending|=s->subscriptions[i].interest&SYS_CLOCK_CONFIG;
}
bool sys_clock_snapshot(const sys_state *s,uint64_t now,sys_clock_state *out){
    if(!s||!out)return false;
    *out=(sys_clock_state){.utc_offset=s->utc_offset,.revision=s->clock_revision,.health=s->clock_health};
    const sys_clock_anchor *a=s->clock.source?&s->clock:&s->pc_clock;
    if(!a->source||now<a->mono_us)return false;
    uint64_t delta=now-a->mono_us;
    uint64_t seconds=delta/1000000;
    int32_t micros=a->microseconds+(int32_t)(delta%1000000);
    if(micros>=1000000){micros-=1000000;seconds++;}
    if(seconds>(uint64_t)(INT64_MAX-a->seconds)){out->health=SYS_CLOCK_OUT_OF_RANGE;return false;}
    out->seconds=a->seconds+(int64_t)seconds;out->microseconds=micros;
    out->source=a->source;out->trusted=a->source!=SYS_CLOCK_PC;out->valid=true;out->health=SYS_CLOCK_OK;
    return true;
}
static bool same_clock(sys_clock_state a,sys_clock_state b){
    return a.health==b.health&&a.valid==b.valid&&a.source==b.source&&a.seconds==b.seconds&&
        a.microseconds==b.microseconds;
}
sys_result sys_clock_update(sys_state *s,sys_clock_anchor a){
    if(!s||(a.source!=SYS_CLOCK_NONE&&a.source!=SYS_CLOCK_RTC&&a.source!=SYS_CLOCK_SNTP)||
       (a.source&&(a.seconds<INT64_C(946684800)||a.microseconds<0||a.microseconds>=1000000)))
        return SYS_INVALID;
    sys_clock_state before,after;
    sys_clock_snapshot(s,a.mono_us,&before);
    s->clock=a;
    s->clock_health=a.source?SYS_CLOCK_OK:SYS_CLOCK_UNSET;
    sys_clock_snapshot(s,a.mono_us,&after);
    if(!same_clock(before,after))publish_clock(s);
    return SYS_OK;
}
sys_result sys_clock_fail(sys_state *s,uint64_t now,sys_clock_health health){
    if(!s||(health!=SYS_CLOCK_UNAVAILABLE&&health!=SYS_CLOCK_OUT_OF_RANGE))return SYS_INVALID;
    sys_clock_state before,after;sys_clock_snapshot(s,now,&before);
    s->clock=(sys_clock_anchor){0};s->clock_health=health;
    sys_clock_snapshot(s,now,&after);
    if(!same_clock(before,after))publish_clock(s);
    return SYS_OK;
}
sys_result sys_clock_offer_pc(sys_state *s,uint32_t seconds,uint64_t now){
    if(!s||seconds<UINT32_C(1577836800))return SYS_INVALID;
    sys_clock_state before,after;
    sys_clock_snapshot(s,now,&before);
    s->pc_clock=(sys_clock_anchor){.seconds=seconds,.mono_us=now,.source=SYS_CLOCK_PC};
    sys_clock_snapshot(s,now,&after);
    if(!same_clock(before,after))publish_clock(s);
    return SYS_OK;
}
sys_result sys_clock_timezone(sys_state *s,int32_t offset){
    if(!s||offset < -50400||offset>50400)return SYS_INVALID;
    if(s->utc_offset!=offset){s->utc_offset=offset;publish_clock(s);}
    return SYS_OK;
}
void sys_power_refresh(sys_state *s){if(s)s->refresh=true;}
uint64_t sys_power_deadline(const sys_state *s){
    return s&&(s->power_users||s->refresh)?s->power_next:SYS_NEVER;
}
void sys_power_step(sys_state *s,uint64_t now,sys_power_port port,void *ctx){
    if(!s||!port||sys_power_deadline(s)==SYS_NEVER||now<s->power_next)return;
    s->power=port(ctx,now);s->power.sampled=true;s->refresh=false;
    s->power_next=now>UINT64_MAX-SYS_POWER_PERIOD_US?SYS_NEVER:now+SYS_POWER_PERIOD_US;
    int64_t difference=(int64_t)s->power.millivolts-s->announced_mv;
    bool changed=!s->announced||s->power.valid!=s->announced_valid||
        s->power.error!=s->announced_error||(s->power.valid&&(difference>=20||difference<=-20));
    if(!changed)return;
    s->announced=true;s->announced_valid=s->power.valid;
    s->announced_mv=s->power.millivolts;s->announced_error=s->power.error;
    for(unsigned i=0;i<SYS_SUBSCRIPTIONS;i++)
        s->subscriptions[i].pending|=s->subscriptions[i].interest&SYS_POWER;
}
