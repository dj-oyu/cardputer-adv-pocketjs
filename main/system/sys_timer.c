#include "sys_timer.h"
#include <stddef.h>
#include <string.h>
_Static_assert(sizeof(sys_timer_record)<=80,"timer record budget");
_Static_assert(sizeof(sys_timer)<=352,"timer store budget");
static bool timer_text(const char *p,unsigned max){
    if(!p||!*p)return false;
    for(unsigned i=0;i<=max;i++){
        if(!p[i])return true;
        if((unsigned char)p[i]<32||(unsigned char)p[i]>126)return false;
    }
    return false;
}
static void timer_schedule(sys_timer *s){
    s->deadline=UINT64_MAX;
    for(unsigned i=0;i<SYS_TIMER_SLOTS;i++){
        sys_timer_record *r=&s->records[i];
        if(r->due_us&&!r->blocked&&r->due_us<s->deadline)s->deadline=r->due_us;
    }
}
bool sys_timer_read(const sys_timer *s,uint32_t owner,const char *key,sys_timer_record *out){
    if(!s||!owner||!key||!out)return false;
    for(unsigned i=0;i<SYS_TIMER_SLOTS;i++)if(s->records[i].due_us&&s->records[i].owner==owner&&!strcmp(s->records[i].key,key)){
        *out=s->records[i];return true;
    }
    return false;
}
sys_notice_result sys_timer_set(sys_timer *s,uint32_t owner,const char *key,const char *label,uint64_t due){
    if(!s||!owner||!timer_text(key,SYS_TIMER_KEY)||!timer_text(label,SYS_NOTICE_LABEL)||due==UINT64_MAX)return NOTICE_INVALID;
    sys_timer_record *slot=NULL;
    for(unsigned i=0;i<SYS_TIMER_SLOTS;i++){
        sys_timer_record *r=&s->records[i];
        if(r->due_us&&r->owner==owner&&!strcmp(r->key,key)){slot=r;break;}
        if(!r->due_us&&!slot)slot=r;
    }
    if(!due){
        if(slot&&slot->due_us){*slot=(sys_timer_record){0};s->changed=true;timer_schedule(s);}
        return NOTICE_OK;
    }
    if(!slot)return NOTICE_FULL;
    if(slot->due_us==due&&slot->owner==owner&&!strcmp(slot->key,key)&&!strcmp(slot->label,label))return NOTICE_OK;
    *slot=(sys_timer_record){.owner=owner,.due_us=due};
    memcpy(slot->key,key,strlen(key)+1);memcpy(slot->label,label,strlen(label)+1);
    s->changed=true;timer_schedule(s);return NOTICE_OK;
}
void sys_timer_step(sys_timer *s,sys_notify *notices,uint64_t now,bool notice_changed){
    if(!s||!notices||(!notice_changed&&now<s->deadline))return;
    for(unsigned i=0;i<SYS_TIMER_SLOTS;i++){
        sys_timer_record *r=&s->records[i];
        if(!r->due_us||now<r->due_us||(r->blocked&&!notice_changed))continue;
        uint32_t id;
        if(sys_notify_post(notices,r->owner,0,r->label,0,&id)==NOTICE_OK){
            *r=(sys_timer_record){0};s->changed=true;
        }else if(!r->blocked){r->blocked=true;s->changed=true;}
    }
    timer_schedule(s);
}
void sys_timer_release_owner(sys_timer *s,uint32_t owner){
    if(!s||!owner)return;
    for(unsigned i=0;i<SYS_TIMER_SLOTS;i++)if(s->records[i].owner==owner){
        s->records[i]=(sys_timer_record){0};s->changed=true;
    }
    timer_schedule(s);
}
uint64_t sys_timer_deadline(const sys_timer *s){return s?s->deadline:UINT64_MAX;}
bool sys_timer_take_changed(sys_timer *s){if(!s)return false;bool value=s->changed;s->changed=false;return value;}
