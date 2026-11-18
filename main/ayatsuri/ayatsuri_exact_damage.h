#ifndef AYATSURI_EXACT_DAMAGE_H
#define AYATSURI_EXACT_DAMAGE_H
#include "ayatsuri_dma_damage.h"

/* Each nonblack constant run is represented by [left,right) and RGB565.
 * One FIFO holds the unconsumed tail of the old frame and the new prefix.
 * A full FIFO disables narrowing until a whole new frame is captured. */
enum { AY_EXACT_RUNS=1024, AY_EXACT_HEIGHT=135 };
typedef struct {
    uint32_t runs[AY_EXACT_RUNS], read, write;
    uint8_t counts[AY_EXACT_HEIGHT], next;
    bool valid, compare, overflow;
} ay_exact_history;

static inline ay_damage_span ay_exact_union(ay_damage_span a, ay_damage_span b) {
    if(b.left>=b.right)return a;
    if(a.left>=a.right)return b;
    if(a.left>b.left)a.left=b.left;
    if(a.right<b.right)a.right=b.right;
    return a;
}
static inline void ay_exact_begin(ay_exact_history *s) {
    s->compare=s->valid;
    s->valid=false;
    s->next=0;
    s->overflow=false;
    if(!s->compare)s->read=s->write=0;
}
static inline uint32_t ay_exact_run(const ay_exact_history *s,uint32_t i) {
    return s->runs[i&(AY_EXACT_RUNS-1)];
}
/* Skip equal prefix/suffix runs. Where a boundary of a matching colour
 * moves, their shared interior is unchanged. This gives the exact outer
 * extent, even when new and old internal run counts differ. */
static inline ay_damage_span ay_exact_changes(const ay_exact_history *s,
        uint32_t a,unsigned na,uint32_t b,unsigned nb) {
    while(na && nb && ay_exact_run(s,a)==ay_exact_run(s,b)){++a;++b;--na;--nb;}
    if(!na && !nb)return (ay_damage_span){0,0};
    while(na && nb && ay_exact_run(s,a+na-1)==ay_exact_run(s,b+nb-1)){--na;--nb;}
    uint32_t first=ay_exact_run(s,na?a:b), other=ay_exact_run(s,nb?b:a);
    uint32_t last=ay_exact_run(s,na?a+na-1:b+nb-1), tail=ay_exact_run(s,nb?b+nb-1:a+na-1);
    unsigned l=(uint8_t)first, r=(uint8_t)(last>>8);
    if(na && nb && (first&0xffff00ffu)==(other&0xffff00ffu)){
        unsigned x=(uint8_t)(first>>8),y=(uint8_t)(other>>8);l=x<y?x:y;
    }else if(l>(uint8_t)other)l=(uint8_t)other;
    if(na && nb && (last&0xffffff00u)==(tail&0xffffff00u)){
        unsigned x=(uint8_t)last,y=(uint8_t)tail;r=x>y?x:y;
    }else if(r<(uint8_t)(tail>>8))r=(uint8_t)(tail>>8);
    return (ay_damage_span){(uint8_t)(l&~15u),(uint8_t)((r+15u)&~15u)};
}
/* 'current' is the conservative model extent before fixed UI decoration.
 * 'fallback' is the normal board damage window (includes old extent).
 * The caller forces it for capture, overlays, label or external writes. */
static inline ay_damage_span ay_exact_row(ay_exact_history *s,unsigned y,const uint16_t *pixels,
        ay_damage_span current,ay_damage_span fallback,bool force) {
    if(s->overflow)return fallback;
    if(y!=s->next || y>=AY_EXACT_HEIGHT || current.left>current.right || current.right>240){
        s->overflow=true;return fallback;
    }
    unsigned prior=s->compare?s->counts[y]:0;
    uint32_t start=s->write;
    for(unsigned x=current.left;x<current.right;){
        unsigned begin=x;uint16_t c=pixels[x++];
        while(x<current.right && pixels[x]==c)++x;
        if(!c)continue;
        if(s->write-s->read>=AY_EXACT_RUNS){s->overflow=true;return fallback;}
        s->runs[s->write++&(AY_EXACT_RUNS-1)]=begin|(x<<8)|((uint32_t)c<<16);
    }
    unsigned count=s->write-start;
    ay_damage_span change=force || !s->compare ? fallback : ay_exact_changes(s,s->read,prior,start,count);
    s->read+=prior;s->counts[y]=(uint8_t)count;s->next++;
    return change;
}
static inline void ay_exact_finish(ay_exact_history *s,bool completed) {
    s->valid=completed && !s->overflow && s->next==AY_EXACT_HEIGHT;
}
#endif
