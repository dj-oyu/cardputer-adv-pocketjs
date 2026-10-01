#include "ksn_procedural_surface.h"
#include <stdlib.h>
#include <string.h>

void ksn_proc_surface_init(ksn_proc_surface *s){if(s)memset(s,0,sizeof *s);}
static bool valid_frame(const ksn_proc_frame *frame){
    if(!frame||!frame->ready||frame->count>KSN_PROC_SEGMENTS)return false;
    unsigned steps=0,i=0,cost;
    ksn_proc_segment geometry;
    ksn_proc_seg_kind kind;
    /* Walks geometry only: an extended entry's parameters are not coordinates. */
    while(ksn_proc_frame_next(frame,&i,&geometry,&kind,&cost)){
        const ksn_proc_segment *seg=&geometry;
        if(seg->x0 < -480||seg->x0 > 720||seg->x1 < -480||seg->x1 > 720||
           seg->y0 < -480||seg->y0 > 720||seg->y1 < -480||seg->y1 > 720)return false;
        steps+=cost;
        if(steps>KSN_PROC_RASTER_STEPS)return false;
    }
    return i==frame->count&&steps==frame->raster_steps;
}
static void mark_frame(ksn_proc_damage *d,const ksn_proc_frame *f){
    if(!f)return;
    unsigned i=0,cost;
    ksn_proc_segment geometry;
    ksn_proc_seg_kind kind;
    while(ksn_proc_frame_next(f,&i,&geometry,&kind,&cost)){
        const ksn_proc_segment *seg=&geometry;
        int left=seg->x0<seg->x1?seg->x0:seg->x1;
        int right=seg->x0>seg->x1?seg->x0:seg->x1;
        int top=seg->y0<seg->y1?seg->y0:seg->y1;
        int bottom=seg->y0>seg->y1?seg->y0:seg->y1;
        if(right<0||left>=KSN_PROC_W||bottom<0||top>=KSN_PROC_H)continue;
        if(left<0)left=0;
        if(right>=KSN_PROC_W)right=KSN_PROC_W-1;
        if(top<0)top=0;
        if(bottom>=KSN_PROC_H)bottom=KSN_PROC_H-1;
        for(int band=top/8;band<=bottom/8;band++){
            uint32_t bit=1u<<band;
            if(!(d->bands&bit)){
                d->bands|=bit;d->x0[band]=(int16_t)left;d->x1[band]=(int16_t)(right+1);
            }else{
                if(left<d->x0[band])d->x0[band]=(int16_t)left;
                if(right+1>d->x1[band])d->x1[band]=(int16_t)(right+1);
            }
        }
    }
}
uint32_t ksn_proc_surface_stage(ksn_proc_surface *s,const ksn_proc_frame *source){
    if(!s||s->pending||s->repair_required||!valid_frame(source))return 0;
    unsigned slot=s->committed_slot^1u;
    ksn_proc_frame *dest=&s->slots[slot];
    dest->ready=false;
    dest->count=source->count;
    dest->raster_steps=source->raster_steps;
    dest->ext=source->ext;
    memcpy(dest->segments,source->segments,(size_t)source->count*sizeof source->segments[0]);
    dest->ready=true;
    memset(&s->pending_damage,0,sizeof s->pending_damage);
    if(s->has_committed)mark_frame(&s->pending_damage,&s->slots[s->committed_slot]);
    mark_frame(&s->pending_damage,dest);
    s->pending_slot=(uint8_t)slot;s->pending=true;
    if(++s->generation==0)++s->generation;
    return s->generation;
}
const ksn_proc_damage *ksn_proc_surface_pending_damage(const ksn_proc_surface *s){
    return s&&s->pending?&s->pending_damage:0;
}
const ksn_proc_frame *ksn_proc_surface_committed_frame(const ksn_proc_surface *s){
    return s&&s->has_committed?&s->slots[s->committed_slot]:0;
}
const ksn_proc_frame *ksn_proc_surface_pending_frame(const ksn_proc_surface *s){
    return s&&s->pending?&s->slots[s->pending_slot]:0;
}
bool ksn_proc_surface_finish(ksn_proc_surface *s,uint32_t generation,bool transferred){
    if(!s||!s->pending||!generation||generation!=s->generation)return false;
    s->pending=false;
    if(transferred){
        s->committed_slot=s->pending_slot;s->has_committed=true;s->repair_required=false;
    }else s->repair_required=true;
    return true;
}
bool ksn_proc_surface_needs_repair(const ksn_proc_surface *s){return s&&s->repair_required;}
void ksn_proc_surface_repair_done(ksn_proc_surface *s){if(s&&!s->pending)s->repair_required=false;}
static bool render(const ksn_proc_frame *frame,ksn_proc_backdrop_load backdrop,
                   void *ctx,uint16_t *pixels,int y,int height){
    if((frame&&!frame->ready)||!backdrop||!pixels||y<0||height<0||
       y>KSN_PROC_H||height>KSN_PROC_H-y)return false;
    if(!backdrop(ctx,pixels,y,height))return false;
    return !frame||ksn_proc_render_band(frame,pixels,y,height);
}
bool ksn_proc_surface_render_pending(const ksn_proc_surface *s,
    ksn_proc_backdrop_load backdrop,void *ctx,uint16_t *pixels,int y,int height){
    return s&&s->pending&&render(&s->slots[s->pending_slot],backdrop,ctx,pixels,y,height);
}
bool ksn_proc_surface_render_committed(const ksn_proc_surface *s,
    ksn_proc_backdrop_load backdrop,void *ctx,uint16_t *pixels,int y,int height){
    return s&&render(s->has_committed?&s->slots[s->committed_slot]:0,
                     backdrop,ctx,pixels,y,height);
}
