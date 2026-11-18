#ifndef AYATSURI_EXACT_DAMAGE_H
#define AYATSURI_EXACT_DAMAGE_H
#include "ayatsuri_dma_damage.h"

/* Each nonblack constant run is represented by [left,right) and RGB565.
 * One FIFO holds the unconsumed tail of the old frame and the new prefix.
 * A full FIFO disables narrowing until a whole new frame is captured. */
#if defined(AY_COMPANION_COMPACT_HISTORY) && AY_COMPANION_COMPACT_HISTORY
#include <string.h>
enum { AY_EXACT_BYTES=8192, AY_EXACT_HEIGHT=135 };
typedef struct {
    uint8_t bytes[AY_EXACT_BYTES];
    uint16_t counts[AY_EXACT_HEIGHT];
    uint32_t read,write;
    uint8_t next;
    bool valid,compare,overflow;
} ay_exact_history;
#else
#ifdef AY_COMPANION_COLORED_LIGHT
enum { AY_EXACT_RUNS=4096, AY_EXACT_HEIGHT=135 };
#else
enum { AY_EXACT_RUNS=1024, AY_EXACT_HEIGHT=135 };
#endif
typedef struct {
    uint32_t runs[AY_EXACT_RUNS], read, write;
    uint8_t counts[AY_EXACT_HEIGHT], next;
    bool valid, compare, overflow;
} ay_exact_history;

#endif

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
#if defined(AY_COMPANION_COMPACT_HISTORY) && AY_COMPANION_COMPACT_HISTORY
static inline unsigned ay_exact_get(const ay_exact_history *s,uint32_t *pos) {
    return s->bytes[(*pos)++ & (AY_EXACT_BYTES-1)];
}
static inline bool ay_exact_encoded_equal(const ay_exact_history *s,uint32_t a,uint32_t b,
                                            unsigned size) {
    while(size) {
        unsigned x=a&(AY_EXACT_BYTES-1), y=b&(AY_EXACT_BYTES-1);
        unsigned contiguous=AY_EXACT_BYTES-(x>y?x:y);
        if(contiguous>size)contiguous=size;
        if(memcmp(s->bytes+x,s->bytes+y,contiguous))return false;
        a+=contiguous;b+=contiguous;size-=contiguous;
    }
    return true;
}
typedef struct { unsigned left,right,color; } ay_exact_segment;
static inline ay_exact_segment ay_exact_decode(const ay_exact_history *s,uint32_t *pos,
                                               uint32_t end,unsigned previous) {
    if(*pos==end)return (ay_exact_segment){240,240,0};
    unsigned index=*pos&(AY_EXACT_BYTES-1);
    unsigned header=s->bytes[index];
    if((header&15)!=15 && header<240 && index<=AY_EXACT_BYTES-3) {
        const uint8_t *token=s->bytes+index;
        unsigned left=previous+(header>>4);
        unsigned color=token[1]|((unsigned)token[2]<<8);
        *pos+=3;
        return (ay_exact_segment){left,left+(header&15)+1,color};
    }
    unsigned control=ay_exact_get(s,pos), gap=control>>4, length=control&15;
    if(gap==15)gap=ay_exact_get(s,pos);
    if(length==15)length=ay_exact_get(s,pos);
    unsigned color=ay_exact_get(s,pos);
    color|=ay_exact_get(s,pos)<<8;
    unsigned left=previous+gap;
    return (ay_exact_segment){left,left+length+1,color};
}
/* Compare the constant-colour intervals between successive run boundaries. */
static inline ay_damage_span ay_exact_compact_changes(const ay_exact_history *s,
        uint32_t a,uint32_t end_a,uint32_t b,uint32_t end_b) {
    if(end_a-a==end_b-b && ay_exact_encoded_equal(s,a,b,end_a-a))return (ay_damage_span){0,0};
    ay_exact_segment old=ay_exact_decode(s,&a,end_a,0);
    ay_exact_segment cur=ay_exact_decode(s,&b,end_b,0);
    unsigned x=old.left<cur.left?old.left:cur.left, left=240, right=0;
    while(x<240) {
        bool in_old=x>=old.left, in_cur=x>=cur.left;
        unsigned old_boundary=in_old?old.right:old.left;
        unsigned cur_boundary=in_cur?cur.right:cur.left;
        unsigned next=old_boundary<cur_boundary?old_boundary:cur_boundary;
        if((in_old?old.color:0)!=(in_cur?cur.color:0)) {
            if(left==240)left=x;
            right=next;
        }
        x=next;
        if(x==old.right)old=ay_exact_decode(s,&a,end_a,old.right);
        if(x==cur.right)cur=ay_exact_decode(s,&b,end_b,cur.right);
    }
    if(right==0)return (ay_damage_span){0,0};
    return (ay_damage_span){(uint8_t)(left&~15u),(uint8_t)((right+15u)&~15u)};
}
static inline ay_damage_span ay_exact_row(ay_exact_history *s,unsigned y,const uint16_t *pixels,
        ay_damage_span current,ay_damage_span fallback,bool force) {
    if(s->overflow)return fallback;
    if(y!=s->next || y>=AY_EXACT_HEIGHT || current.left>current.right || current.right>240){
        s->overflow=true;return fallback;
    }
    uint32_t old_end=s->read+(s->compare?s->counts[y]:0), start=s->write;
    uint32_t write=start;
    unsigned previous=0;
    for(unsigned x=current.left;x<current.right;) {
        unsigned left=x;uint16_t color=pixels[x++];
        while(x<current.right && pixels[x]==color)++x;
        if(!color)continue;
        unsigned gap=left-previous, length=x-left-1;
        unsigned control=((gap<15?gap:15)<<4)|(length<15?length:15);
        unsigned index=write&(AY_EXACT_BYTES-1);
        if(gap<15 && length<15 && index<=AY_EXACT_BYTES-3) {
            if(write-s->read>AY_EXACT_BYTES-3){s->overflow=true;return fallback;}
            uint8_t *token=s->bytes+index;
            token[0]=(uint8_t)((gap<<4)|length);
            token[1]=(uint8_t)color;
            token[2]=(uint8_t)(color>>8);
            write+=3;
        } else {
            unsigned size=3+(gap>=15)+(length>=15);
            if(write-s->read>AY_EXACT_BYTES-size){s->overflow=true;return fallback;}
            s->bytes[write++&(AY_EXACT_BYTES-1)]=(uint8_t)control;
            if(gap>=15)s->bytes[write++&(AY_EXACT_BYTES-1)]=(uint8_t)gap;
            if(length>=15)s->bytes[write++&(AY_EXACT_BYTES-1)]=(uint8_t)length;
            s->bytes[write++&(AY_EXACT_BYTES-1)]=(uint8_t)color;
            s->bytes[write++&(AY_EXACT_BYTES-1)]=(uint8_t)(color>>8);
        }
        previous=x;
    }

    s->write=write;
    ay_damage_span change=force || !s->compare ? fallback :
        ay_exact_compact_changes(s,s->read,old_end,start,s->write);
    s->read=old_end;s->counts[y]=(uint16_t)(s->write-start);s->next++;
    return change;
}
#else
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
#endif
static inline void ay_exact_finish(ay_exact_history *s,bool completed) {
    s->valid=completed && !s->overflow && s->next==AY_EXACT_HEIGHT;
}
#endif
