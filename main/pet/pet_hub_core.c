#include "pet_hub_core.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

static uint32_t u32(const uint8_t *p) {
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static unsigned u16(const uint8_t *p) { return p[0]|(unsigned)p[1]<<8; }
uint32_t pet_crc(const uint8_t *p, unsigned n) {
    uint32_t c=UINT32_MAX;
    while(n--) { c^=*p++; for(unsigned i=0;i<8;i++)c=(c>>1)^((0u-(c&1))&0xedb88320u); }
    return c^UINT32_MAX;
}
void pet_hub_defaults(pet_hub_t *h) {
    memset(h,0,sizeof(*h));h->saved.magic=PET_HUB_MAGIC;
    h->saved.wake_minute=-1;h->saved.utc_offset=9*3600;
    for(unsigned p=0;p<2;p++)for(unsigned w=0;w<2;w++)h->saved.usage[p].used[w]=-1;
}
bool pet_hub_notify(pet_hub_t *h, const char *label) {
    uint32_t id;
    return sys_notify_post(h->notifications,PET_NOTICE_OWNER,0,label,0,&id)==NOTICE_OK;
}
bool pet_hub_take(pet_hub_t *h, char label[PET_LABEL_CHARS+1]) {
    /* Destructive legacy queue adapter. The firmware presenter uses active
     * snapshots and explicit acknowledgement instead. */
    sys_notify_step(h->notifications,0);sys_notice notice;
    if(!sys_notify_active(h->notifications,&notice))return false;
    memcpy(label,notice.label,PET_LABEL_CHARS+1);
    return sys_notify_ack(h->notifications,PET_NOTICE_OWNER,notice.id)==NOTICE_OK;
}
bool pet_hub_packet(pet_hub_t *h, const uint8_t d[PET_WIRE_BYTES]) {
    if(d[0]!=1||d[1]!=1||d[2]>1||(d[3]&~7)||pet_crc(d,44)!=u32(d+44))return false;
    uint32_t stream=u32(d+8), stamp=u32(d+12), sequence=u32(d+4);
    int32_t offset=(int32_t)u32(d+36);
    uint64_t tokens=(uint64_t)u32(d+16)|((uint64_t)u32(d+20)<<32);
    uint32_t sent=u32(d+40);
    if(!stream||stamp<1577836800u||sent<stamp||sent-stamp>180||offset < -50400||offset>50400||tokens>9007199254740991ULL)return false;
    for(unsigned w=0;w<2;w++)if((d[3]&(2u<<w))&&u16(d+28+2*w)>10000)return false;
    pet_usage_t *p=&h->saved.usage[d[2]];
    if(p->stream==stream&&sequence<=p->sequence)return false;
    if(p->stream!=stream) { memset(p,0,sizeof(*p));p->stream=stream; }
    p->sequence=sequence;p->observed=stamp;h->saved.utc_offset=offset;
    if(d[3]&1) {
        if(p->baseline&&tokens>p->tokens) {
            uint64_t amount=tokens-p->tokens+p->remainder;
            uint32_t *food=&h->saved.food[h->saved.selected];
            uint64_t total=(uint64_t)*food+amount/1000;
            *food=total>UINT32_MAX?UINT32_MAX:(uint32_t)total;
            p->remainder=amount%1000;
        }
        if(!p->baseline||tokens>p->tokens)p->tokens=tokens;
        p->baseline=true;
    }
    for(unsigned w=0;w<2;w++) {
        p->used[w]=(d[3]&(2u<<w))?(int32_t)u16(d+28+2*w):-1;
        uint32_t reset=u32(d+(w?32:24));
        // Missing windows revoke their old timers; past snapshots never re-arm.
        p->reset[w]=p->used[w]>=0&&reset>stamp?reset:0;
    }
    return true;
}
bool pet_hub_timer(pet_hub_t *h, const char *id, const char *label, uint64_t due) {
    if(due>UINT64_MAX/1000)return false;
    return sys_timer_set(h->timers,PET_NOTICE_OWNER,id,label,due*1000)==NOTICE_OK;
}
void pet_hub_bind_wall(pet_hub_t *h,sys_wall *s){
    *s=(sys_wall){.owner=PET_NOTICE_OWNER,.invalid=true};
    static const char *const labels[4]={"CODEX RESET TIME","CODEX SECOND RESET TIME",
        "CLAUDE 5H RESET TIME","CLAUDE WEEK RESET TIME"};
    for(unsigned p=0;p<2;p++)for(unsigned w=0;w<2;w++)
        s->rules[p*2+w]=(sys_wall_rule){.due=&h->saved.usage[p].reset[w],
            .last=&h->saved.usage[p].notified[w],.label=labels[p*2+w]};
    s->rules[4]=(sys_wall_rule){.last=&h->saved.wake_day,.minute=&h->saved.wake_minute,
        .offset=&h->saved.utc_offset,.label="GOOD MORNING!"};
}
bool pet_hub_tick(pet_hub_t *h, uint64_t ms, uint32_t utc) {
    bool dirty=false;
    if(ms<=UINT64_MAX/1000)
        sys_timer_step(h->timers,h->notifications,ms*1000,h->notifications&&h->notifications->changed);
    /* Legacy host adapter. Firmware binds the long-lived scheduler once. */
    sys_wall s;pet_hub_bind_wall(h,&s);
    for(unsigned i=0;i<SYS_WALL_RULES;i++){
        sys_wall_rule *r=&s.rules[i];uint32_t before=*r->last;
        sys_wall_evaluate(r,h->notifications,PET_NOTICE_OWNER,utc,NULL);
        dirty|=before!=*r->last;
    }
    return dirty;
}
