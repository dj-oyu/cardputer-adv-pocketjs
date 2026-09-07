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
    if(h->count==PET_MAX_ALERTS)return false;
    snprintf(h->alerts[(h->read+h->count)%PET_MAX_ALERTS],PET_LABEL_CHARS+1,"%s",label);
    h->count++;return true;
}
bool pet_hub_take(pet_hub_t *h, char label[PET_LABEL_CHARS+1]) {
    if(!h->count)return false;
    memcpy(label,h->alerts[h->read],PET_LABEL_CHARS+1);
    h->read=(h->read+1)%PET_MAX_ALERTS;h->count--;return true;
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
    int slot=-1;
    for(unsigned i=0;i<PET_MAX_TIMERS;i++) {
        if(!strcmp(h->timers[i].id,id)){slot=i;break;}
        if(!h->timers[i].due)slot=i;
    }
    if(slot<0)return due==0;
    pet_timer_t *t=&h->timers[slot];
    snprintf(t->id,sizeof(t->id),"%s",id);
    snprintf(t->label,sizeof(t->label),"%s",label);t->due=due;return true;
}
bool pet_hub_tick(pet_hub_t *h, uint64_t ms, uint32_t utc) {
    bool dirty=false;
    for(unsigned i=0;i<PET_MAX_TIMERS;i++) {
        pet_timer_t *t=&h->timers[i];
        if(t->due&&ms>=t->due&&pet_hub_notify(h,t->label))t->due=0;
    }
    if(!utc)return false;
    for(unsigned p=0;p<2;p++)for(unsigned w=0;w<2;w++) {
        pet_usage_t *u=&h->saved.usage[p];uint32_t r=u->reset[w];
        if(r&&utc>=r&&u->notified[w]!=r) {
            const char *label=p?(w?"CLAUDE WEEK RESET TIME":"CLAUDE 5H RESET TIME"):
                                (w?"CODEX SECOND RESET TIME":"CODEX RESET TIME");
            if(pet_hub_notify(h,label)){u->notified[w]=r;dirty=true;}
        }
    }
    int64_t local=(int64_t)utc+h->saved.utc_offset;
    uint32_t day=(uint32_t)(local/86400), seconds=local%86400;
    if(h->saved.wake_minute>=0&&h->saved.wake_day!=day&&
       seconds/60==(uint32_t)h->saved.wake_minute&&pet_hub_notify(h,"GOOD MORNING!")) {
        h->saved.wake_day=day;dirty=true;
    }
    return dirty;
}
