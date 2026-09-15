#include "sys_notify.h"
#include <stddef.h>
#include <string.h>
static uint32_t last_notice;
_Static_assert(sizeof(sys_notice)<=80,"notice record budget");
_Static_assert(sizeof(sys_notify)<=768,"notice store budget");
static void changed(sys_notify *s){
    s->changed=true;if(s->revision!=UINT32_MAX)s->revision++;
}
static uint64_t next_order(sys_notify *s){
    if(s->next_order==UINT64_MAX){
        uint8_t ranks[SYS_NOTICE_SLOTS]={0};uint64_t count=0;
        for(unsigned i=0;i<SYS_NOTICE_SLOTS;i++)if(s->records[i].phase){
            ranks[i]=1;count++;
            for(unsigned j=0;j<SYS_NOTICE_SLOTS;j++)if(s->records[j].phase&&s->records[j].order<s->records[i].order)ranks[i]++;
        }
        for(unsigned i=0;i<SYS_NOTICE_SLOTS;i++)s->records[i].order=ranks[i];
        s->next_order=count;
    }
    return ++s->next_order;
}
static unsigned waiting(const sys_notify *s){
    unsigned n=0;for(unsigned i=0;i<SYS_NOTICE_SLOTS;i++)
        n+=s->records[i].phase==NOTICE_QUEUED||s->records[i].phase==NOTICE_SNOOZED;
    return n;
}
static sys_notice *notice_find(sys_notify *s,uint32_t owner,uint32_t id){
    if(s&&id)for(unsigned i=0;i<SYS_NOTICE_SLOTS;i++)
        if(s->records[i].id==id&&s->records[i].owner==owner)return &s->records[i];
    return NULL;
}
static void schedule(sys_notify *s){
    uint64_t due=UINT64_MAX;bool active=false,queued=false;
    s->active_slot=0;
    for(unsigned i=0;i<SYS_NOTICE_SLOTS;i++){
        sys_notice *r=&s->records[i];
        active|=r->phase==NOTICE_ACTIVE;queued|=r->phase==NOTICE_QUEUED;
        if(r->phase==NOTICE_ACTIVE)s->active_slot=(uint8_t)(i+1);
        if(r->phase==NOTICE_SNOOZED&&r->wake_us<due)due=r->wake_us;
        if((r->phase==NOTICE_ACTIVE||r->phase==NOTICE_QUEUED)&&r->expires_us&&r->expires_us<due)
            due=r->expires_us;
    }
    s->deadline=!active&&queued?0:due;
}
sys_notice_result sys_notify_post(sys_notify *s,uint32_t owner,uint32_t key,
    const char *label,uint64_t expiry,uint32_t *id){
    if(!s||!owner||!label||!id)return NOTICE_INVALID;
    size_t n=0;while(n<=SYS_NOTICE_LABEL&&label[n]){
        if((unsigned char)label[n]<32||(unsigned char)label[n]>126)return NOTICE_INVALID;
        n++;
    }
    if(!n||n>SYS_NOTICE_LABEL)return NOTICE_INVALID;
    if(key)for(unsigned i=0;i<SYS_NOTICE_SLOTS;i++){
        sys_notice *r=&s->records[i];
        if(r->phase&&r->owner==owner&&r->key==key){*id=r->id;return NOTICE_OK;}
    }
    if(waiting(s)>=SYS_NOTICE_WAITING||last_notice==UINT32_MAX)return NOTICE_FULL;
    for(unsigned i=0;i<SYS_NOTICE_SLOTS;i++)if(!s->records[i].phase){
        sys_notice *r=&s->records[i];
        uint64_t order=next_order(s);
        *r=(sys_notice){.id=++last_notice,.owner=owner,.key=key,.order=order,
            .expires_us=expiry,.phase=NOTICE_QUEUED};
        memcpy(r->label,label,n+1);*id=r->id;changed(s);schedule(s);return NOTICE_OK;
    }
    return NOTICE_FULL;
}
bool sys_notify_active(const sys_notify *s,sys_notice *out){
    if(!s||!out||!s->active_slot)return false;
    *out=s->records[s->active_slot-1];return true;
}
sys_notice_result sys_notify_cancel(sys_notify *s,uint32_t owner,uint32_t id){
    sys_notice *r=notice_find(s,owner,id);if(!r)return NOTICE_GONE;
    *r=(sys_notice){0};changed(s);schedule(s);return NOTICE_OK;
}
sys_notice_result sys_notify_ack(sys_notify *s,uint32_t owner,uint32_t id){
    sys_notice *r=notice_find(s,owner,id);if(!r)return NOTICE_GONE;
    if(r->phase!=NOTICE_ACTIVE)return NOTICE_INVALID;
    return sys_notify_cancel(s,owner,id);
}
sys_notice_result sys_notify_snooze(sys_notify *s,uint32_t owner,uint32_t id,uint64_t wake){
    sys_notice *r=notice_find(s,owner,id);if(!r)return NOTICE_GONE;
    if(r->phase!=NOTICE_ACTIVE||!wake||wake==UINT64_MAX)return NOTICE_INVALID;
    if(waiting(s)>=SYS_NOTICE_WAITING)return NOTICE_FULL;
    r->phase=NOTICE_SNOOZED;r->wake_us=wake;changed(s);schedule(s);return NOTICE_OK;
}
void sys_notify_release_owner(sys_notify *s,uint32_t owner){
    if(!s||!owner)return;
    for(unsigned i=0;i<SYS_NOTICE_SLOTS;i++)if(s->records[i].owner==owner){
        s->records[i]=(sys_notice){0};changed(s);
    }
    schedule(s);
}
void sys_notify_step(sys_notify *s,uint64_t now){
    if(!s||now<sys_notify_deadline(s))return;
    sys_notice *first=NULL;bool active=false;
    for(unsigned i=0;i<SYS_NOTICE_SLOTS;i++){
        sys_notice *r=&s->records[i];
        if(r->phase==NOTICE_SNOOZED&&now>=r->wake_us){
            r->order=next_order(s);r->phase=NOTICE_QUEUED;changed(s);
        }
        if((r->phase==NOTICE_QUEUED||r->phase==NOTICE_ACTIVE)&&r->expires_us&&now>=r->expires_us){
            *r=(sys_notice){0};changed(s);
        }
        active|=r->phase==NOTICE_ACTIVE;
        if(r->phase==NOTICE_QUEUED&&(!first||r->order<first->order))first=r;
    }
    if(!active&&first){first->phase=NOTICE_ACTIVE;changed(s);}
    schedule(s);
}
uint64_t sys_notify_deadline(const sys_notify *s){return s?s->deadline:UINT64_MAX;}
bool sys_notify_take_changed(sys_notify *s){if(!s)return false;bool value=s->changed;s->changed=false;return value;}
