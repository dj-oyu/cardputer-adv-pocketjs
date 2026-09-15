#include "sys_ringer.h"
uint64_t sys_ringer_deadline(const sys_ringer *s,const sys_notify *notices){
    sys_notice active;
    if(!s||!sys_notify_active(notices,&active))return UINT64_MAX;
    return s->id==active.id&&s->order==active.order?s->next:0;
}
bool sys_ringer_poll(sys_ringer *s,const sys_notify *notices,uint64_t now){
    if(!s)return false;
    sys_notice active;
    if(!sys_notify_active(notices,&active)){*s=(sys_ringer){0};return false;}
    if(s->id!=active.id||s->order!=active.order){
        s->id=active.id;s->order=active.order;s->next=now;
        s->until=now>UINT64_MAX-30000000?UINT64_MAX:now+30000000;
    }
    if(now>=s->until){s->next=UINT64_MAX;return false;}
    if(now<s->next)return false;
    s->next=s->until-now<=2000000?UINT64_MAX:now+2000000;
    return true;
}
