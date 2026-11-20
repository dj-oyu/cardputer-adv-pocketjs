#ifndef AYATSURI_DMA_DAMAGE_H
#define AYATSURI_DMA_DAMAGE_H
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

enum { AY_DAMAGE_WIDTH=240, AY_DAMAGE_BANDS=17, AY_DAMAGE_LABEL=48 };
/* Half-open screen x. Empty is [0,0). */
typedef struct {uint8_t left,right;} ay_damage_span;
typedef struct {
    ay_damage_span bands[AY_DAMAGE_BANDS];
    char label[AY_DAMAGE_LABEL];
    uint32_t panel_epoch;
    bool known,overlay;
} ay_damage_history;
typedef struct {
    ay_damage_span bands[AY_DAMAGE_BANDS];
    bool full,header,overlay;
} ay_damage_frame;

static inline ay_damage_frame ay_damage_begin(const ay_damage_history *h,uint32_t epoch,
                                               const char *label,bool overlay,bool force) {
    ay_damage_frame f={0};
    f.full=force || !h->known || epoch==UINT32_MAX || h->panel_epoch!=epoch;
    f.header=!h->known || strcmp(h->label,label)!=0;
    f.overlay=overlay;
    return f;
}

static inline ay_damage_span ay_damage_plan(const ay_damage_history *h,ay_damage_frame *f,
                                             unsigned band,ay_damage_span current) {
    f->bands[band]=current;
    if (f->full || (band<4 && f->header) || (band<6 && (f->overlay || h->overlay)))
        return (ay_damage_span){0,AY_DAMAGE_WIDTH};
    ay_damage_span old=h->bands[band];
    if (old.right>old.left) {
        if (current.right<=current.left) current=old;
        else {
            if (old.left<current.left) current.left=old.left;
            if (old.right>current.right) current.right=old.right;
        }
    }
    if (current.right>current.left) {
        current.left=(uint8_t)(current.left&~15u);
        current.right=(uint8_t)((current.right+15u)&~15u);
    }
    return current;
}

static inline void ay_damage_finish(ay_damage_history *h,const ay_damage_frame *f,
                                     uint32_t epoch,const char *label,bool completed) {
    h->known=false;
    if (!completed || epoch==UINT32_MAX || strlen(label)>=sizeof h->label) return;
    memcpy(h->bands,f->bands,sizeof h->bands);
    strcpy(h->label,label);
    h->overlay=f->overlay;
    h->panel_epoch=epoch;
    h->known=true;
}
#endif
