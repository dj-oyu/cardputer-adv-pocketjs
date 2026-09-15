#include "sys_state.h"
#include <stddef.h>
/* Process-lifetime IDs fail closed at exhaustion, even across service reuse. */
static uint32_t last_subscription;
_Static_assert(sizeof(sys_subscription)<=16,"subscription budget");
_Static_assert(sizeof(sys_state)<=256,"power and subscriptions budget");
static sys_subscription *find(sys_state *s,sys_sub id){
    if(s&&id.value)for(unsigned i=0;i<SYS_SUBSCRIPTIONS;i++)
        if(s->subscriptions[i].id==id.value)return &s->subscriptions[i];
    return NULL;
}
sys_result sys_subscribe(sys_state *s,uint32_t interest,sys_sub *out){
    if(!s||!out||(interest&~SYS_POWER))return SYS_INVALID;
    if(last_subscription==UINT32_MAX)return SYS_FULL;
    for(unsigned i=0;i<SYS_SUBSCRIPTIONS;i++)if(!s->subscriptions[i].id){
        s->subscriptions[i]=(sys_subscription){++last_subscription,interest,interest};
        s->power_users+=(interest&SYS_POWER)!=0;
        *out=(sys_sub){last_subscription};return SYS_OK;
    }
    return SYS_FULL;
}
sys_result sys_set_interest(sys_state *s,sys_sub id,uint32_t interest){
    if(interest&~SYS_POWER)return SYS_INVALID;
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
