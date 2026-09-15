#include "sys_wall.h"
void sys_wall_invalidate(sys_wall *s){if(s)s->invalid=true;}
uint64_t sys_wall_evaluate(const sys_wall_rule *r,sys_notify *notices,uint32_t owner,
                          uint32_t utc,bool *blocked){
    if(!r||!r->last||!r->label||!utc)return SYS_NEVER;
    uint64_t next=SYS_NEVER;uint32_t mark;
    if(r->due){
        mark=*r->due;
        if(!mark||*r->last==mark)return SYS_NEVER;
        if(utc<mark)return mark;
    }else{
        if(!r->minute||!r->offset||*r->minute<0||*r->minute>=1440||
           *r->offset < -50400||*r->offset>50400)return SYS_NEVER;
        int64_t local=(int64_t)utc+*r->offset;
        if(local<0)return SYS_NEVER;
        mark=(uint32_t)(local/86400);
        int64_t target=(int64_t)mark*86400+*r->minute*60-*r->offset;
        if(*r->last>=mark)
            return ((uint64_t)*r->last+1)*86400+*r->minute*60-*r->offset;
        if((int64_t)utc<target)return (uint64_t)target;
        if((int64_t)utc>=target+60)return (uint64_t)(target+86400);
        next=(uint64_t)(target+60); /* Full queue may retry only in this minute. */
    }
    uint32_t id;
    if(sys_notify_post(notices,owner,0,r->label,0,&id)==NOTICE_OK){
        *r->last=mark;
        if(!r->due)next=(uint64_t)(((int64_t)mark+1)*86400+*r->minute*60-*r->offset);
    }else if(blocked)*blocked=true;
    return next;
}
uint64_t sys_wall_deadline(const sys_wall *s,const sys_state *clock,const sys_notify *notices){
    if(!s||!s->owner)return SYS_NEVER;
    if(s->invalid||s->clock_revision!=clock->clock_revision||
       (s->blocked&&s->notice_revision!=notices->revision))return 0;
    return s->next;
}
void sys_wall_step(sys_wall *s,const sys_state *clock,sys_notify *notices,uint64_t now){
    if(!s||sys_wall_deadline(s,clock,notices)==SYS_NEVER||now<sys_wall_deadline(s,clock,notices))return;
    s->invalid=false;s->blocked=false;s->next=SYS_NEVER;
    s->clock_revision=clock->clock_revision;
    sys_clock_state wall;
    if(sys_clock_snapshot(clock,now,&wall)&&wall.seconds>0&&wall.seconds<=UINT32_MAX){
        uint64_t next=SYS_NEVER;
        for(unsigned i=0;i<SYS_WALL_RULES;i++){
            sys_wall_rule *r=&s->rules[i];uint32_t before=r->last?*r->last:0;
            uint64_t due=sys_wall_evaluate(r,notices,s->owner,(uint32_t)wall.seconds,&s->blocked);
            if(r->last&&before!=*r->last)s->changed=true;
            if(due<next)next=due;
        }
        /* This compatibility schedule accepts uint32 epochs only. A saved
         * future watermark must not overflow the microsecond conversion. */
        if(next<=UINT32_MAX){
            uint64_t delta=next>(uint64_t)wall.seconds?
                (next-(uint64_t)wall.seconds)*1000000-(uint32_t)wall.microseconds:0;
            s->next=delta>SYS_NEVER-now?SYS_NEVER:now+delta;
        }
    }
    s->notice_revision=notices->revision;
}
bool sys_wall_take_changed(sys_wall *s){bool changed=s&&s->changed;if(s)s->changed=false;return changed;}
