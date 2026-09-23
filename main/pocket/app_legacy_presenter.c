#include "app_legacy_presenter.h"
#include "ui/kasane/ksn_p0_probe.h"
#include <stdio.h>
#include <string.h>

static const char *const player_fixed[]={
    "ENTER  PLAY / PAUSE", "LEFT   CHOOSE A FILE", "UP     CHOOSE A FOLDER",
    "RIGHT  NEXT TRACK", "-  =   VOLUME", "ESC    LEAVE THE PLAYER",
    "?  CLOSE", "?: help"
};

ksn_result ksn_presenter_copy_text(char out[KSN_PRESENTER_TEXT_MAX+1u],
                                   uint8_t *out_bytes,const char *src,size_t bytes){
    if(!out||!out_bytes||(!src&&bytes))return KSN_INVALID;
    size_t used=0;
    for(size_t i=0;i<bytes;){
        unsigned lead=(uint8_t)src[i],n=lead<0x80?1:lead>=0xc2&&lead<=0xdf?2:
                         lead>=0xe0&&lead<=0xef?3:lead>=0xf0&&lead<=0xf4?4:0;
        if(!n||i+n>bytes)return KSN_INVALID;
        uint32_t cp=lead;
        if(n>1)cp=lead&((1u<<(7u-n))-1u);
        for(unsigned j=1;j<n;j++){
            uint8_t b=(uint8_t)src[i+j];
            if((b&0xc0u)!=0x80u)return KSN_INVALID;
            cp=(cp<<6)|(b&0x3fu);
        }
        if(cp<0x20u||cp==0x7fu||
           (n==2&&cp<0x80u)||
           (n==3&&(cp<0x800u||(cp>=0xd800u&&cp<=0xdfffu)))||
           (n==4&&(cp<0x10000u||cp>0x10ffffu)))return KSN_INVALID;
        if(used+n>KSN_PRESENTER_TEXT_MAX)break;
        memcpy(out+used,src+i,n);ksn_p0_probe_copy(KSN_P0_MUSIC_PLAN,n);used+=n;i+=n;
    }
    out[used]=0;*out_bytes=(uint8_t)used;return KSN_OK;
}

static int advance(const char *s,unsigned bytes){
    int width=0;
    for(unsigned i=0;i<bytes;){
        unsigned lead=(uint8_t)s[i];
        unsigned n=lead<0x80?1:lead<0xe0?2:lead<0xf0?3:4;
        /* A non-ASCII lead denotes one scalar, including supplementary ones.
         * The caption renderer advances 8 px regardless of UTF-8 byte count. */
        width+=(int)ksn_font_advance(KSN_CAPTION,lead);i+=n;
    }
    return width;
}
static ksn_result item(ksn_presenter_plan *p,ksn_kind kind,ksn_rect bounds,
                       ksn_rgba color,uint8_t text_id){
    if(p->count==KSN_PRESENTER_ITEMS)return KSN_LIMIT;
    p->items[p->count++]=(ksn_presenter_item){.bounds=bounds,.color=color,
        .kind=(uint8_t)kind,.text_id=text_id,.font=KSN_CAPTION,.reveal=UINT8_MAX};
    return KSN_OK;
}
static ksn_result rect(ksn_presenter_plan *p,int x,int y,int w,int h,ksn_rgba c){
    if(w<=0||h<=0)return KSN_INVALID;
    return item(p,KSN_RECT,(ksn_rect){x,y,x+w,y+h},c,0);
}
static const char *text_for(const ksn_presenter_plan *p,uint8_t id,unsigned *bytes){
    if(id<KSN_PRESENTER_SLOTS){*bytes=p->bytes[id];return p->text[id];}
    if(id>=16&&id<24){const char *s=player_fixed[id-16];*bytes=(unsigned)strlen(s);return s;}
    *bytes=0;return NULL;
}
static ksn_result label(ksn_presenter_plan *p,int x,int y,uint8_t id,ksn_rgba c){
    unsigned bytes;const char *s=text_for(p,id,&bytes);
    if(!s||!bytes)return KSN_INVALID;
    int w=advance(s,bytes);if(!w)w=1;
    return item(p,KSN_TEXT,(ksn_rect){x,y,x+w,y+12},c,id);
}
static ksn_result plate(ksn_presenter_plan *p,int x,int y,uint8_t id,ksn_rgba color){
    unsigned bytes;const char *s=text_for(p,id,&bytes);
    if(!s)return KSN_INVALID;
    if(!bytes)return KSN_OK;
    ksn_result r=rect(p,x,y,advance(s,bytes)+8,16,0x000000ffu);
    return r==KSN_OK?label(p,x+4,y+2,id,color):r;
}
static ksn_result make_player(const ksn_presenter_values *v,uint16_t w,uint16_t h,
                              ksn_presenter_plan *p){
    if(w<25||h<28)return KSN_INVALID;
    if(v->help){
        ksn_result r=rect(p,0,0,w,h,0x000000ffu);
        for(unsigned i=0;r==KSN_OK&&i<6;i++)
            r=label(p,14,14+(int)i*18,(uint8_t)(i+16),0xe2f0ffffu);
        if(r==KSN_OK)r=label(p,14,(int)h-18,22,0x6e8ca5ffu);
        return r;
    }
    memcpy(p->text,v->text,sizeof(p->text));memcpy(p->bytes,v->bytes,sizeof(p->bytes));
    ksn_p0_probe_copy(KSN_P0_MUSIC_PLAN,sizeof(p->text)+sizeof(p->bytes));
    ksn_result r=plate(p,8,8,0,0xe2f0ffffu);
    if(r==KSN_OK)r=plate(p,8,28,1,0x96bedcffu);
    int track=(int)w-24,y=(int)h-26;
    if(r==KSN_OK)r=rect(p,12,y,track,2,0x182636ffu);
    if(r!=KSN_OK)return r;
    if(v->duration_ms){
        uint64_t scaled=(uint64_t)v->position_ms*(unsigned)track/v->duration_ms;
        int fill=scaled>(uint64_t)track?track:(int)scaled;
        if(fill>0)r=rect(p,12,y,fill,2,0x78c8ffffu);
    }else if(v->playing){
        const ksn_rgba colors[]={0x78c8ffffu,0x3c78aaffu,0x1e3c5affu};
        unsigned span=(unsigned)track+54u;
        int head=(int)(((uint64_t)v->phase*3u)%span)-54;
        for(int i=0;i<3&&r==KSN_OK;i++){
            int x=head+i*18,width=18;
            if(x<0){width+=x;x=0;}
            if(x+width>track)width=track-x;
            if(width>0)r=rect(p,12+x,y,width,2,colors[i]);
        }
    }
    if(r==KSN_OK)r=plate(p,8,(int)h-20,23,0x6e8ca5ffu);
    return r;
}
ksn_result ksn_presenter_make(ksn_presenter_kind kind,const ksn_presenter_values *v,
                              uint16_t width,uint16_t height,ksn_presenter_plan *out){
    if(!v||!out||!width||!height)return KSN_INVALID;
    *out=(ksn_presenter_plan){0};
    out->background=0x000000ffu;
    return kind==KSN_PRESENTER_MUSIC?make_player(v,width,height,out):KSN_UNSUPPORTED;
}
bool ksn_presenter_equal(const ksn_presenter_plan *a,const ksn_presenter_plan *b){
    if(!a||!b||a->count!=b->count||a->background!=b->background||
       a->patchable!=b->patchable)return false;
    for(unsigned i=0;i<a->count;i++){
        const ksn_presenter_item *x=&a->items[i],*y=&b->items[i];
        if(x->kind!=y->kind||x->text_id!=y->text_id||x->font!=y->font||
           x->radius!=y->radius||x->variant!=y->variant||x->frame!=y->frame||
           x->reveal!=y->reveal||x->color!=y->color||
           x->bounds.x0!=y->bounds.x0||x->bounds.y0!=y->bounds.y0||
           x->bounds.x1!=y->bounds.x1||x->bounds.y1!=y->bounds.y1)return false;
    }
    for(unsigned i=0;i<KSN_PRESENTER_SLOTS;i++)if(a->bytes[i]!=b->bytes[i]||
        memcmp(a->text[i],b->text[i],a->bytes[i])!=0)return false;
    return true;
}
static ksn_rect intersect(ksn_rect a,ksn_rect b){
    if(a.x0<b.x0)a.x0=b.x0;
    if(a.y0<b.y0)a.y0=b.y0;
    if(a.x1>b.x1)a.x1=b.x1;
    if(a.y1>b.y1)a.y1=b.y1;
    if(a.x1<a.x0)a.x1=a.x0;
    if(a.y1<a.y0)a.y1=a.y0;
    return a;
}
ksn_result ksn_presenter_submit(ksn_view *view,ksn_rect viewport,ksn_resource image,
                                const ksn_presenter_plan *plan,
                                ksn_ref refs[KSN_PRESENTER_ITEMS],ksn_tx *out){
    if(!view||!plan||!refs||!out||plan->count>KSN_PRESENTER_ITEMS)return KSN_INVALID;
    ksn_tx tx;ksn_result r=ksn_view_begin(view,KSN_REPLACE,&tx);
    if(r!=KSN_OK)return r;
    r=ksn_view_background(view,tx,plan->background);
    for(unsigned i=0;r==KSN_OK&&i<plan->count;i++){
        const ksn_presenter_item *it=&plan->items[i];
        ksn_draw d={.kind=(ksn_kind)it->kind,.opacity=255};
        d.bounds=(ksn_rect){it->bounds.x0+viewport.x0,it->bounds.y0+viewport.y0,
                            it->bounds.x1+viewport.x0,it->bounds.y1+viewport.y0};
        d.clip=plan->patchable?viewport:intersect(d.bounds,viewport);
        if(d.kind==KSN_RECT||d.kind==KSN_ROUND_RECT){
            d.data.shape.color=it->color;d.data.shape.radius=it->radius;
        }
        else if(d.kind==KSN_TEXT){
            unsigned bytes;const char *s=text_for(plan,it->text_id,&bytes);
            if(!s||!bytes){r=KSN_INVALID;break;}
            d.data.text.utf8=s;d.data.text.bytes=(uint16_t)bytes;
            d.data.text.capacity=plan->patchable&&it->text_id<KSN_PRESENTER_SLOTS?
                                 KSN_PRESENTER_TEXT_MAX:(uint16_t)bytes;
            d.data.text.font=(ksn_font)it->font;
            d.data.text.color=it->color;
        }else if(d.kind==KSN_IMAGE){
            if(!image.value){r=KSN_INVALID;break;}
            d.data.image.resource=image;
            d.data.image.variant=it->variant;d.data.image.frame=it->frame;
            d.data.image.scale=KSN_IMAGE_STRETCH;
            d.data.image.source_width=64;d.data.image.source_height=64;
        }else{r=KSN_INVALID;break;}
        ksn_ref ref;r=ksn_view_add(view,tx,&d,&ref);
        if(r==KSN_OK)refs[i]=ref;
        if(r==KSN_OK&&d.kind==KSN_TEXT&&it->reveal!=UINT8_MAX){
            ksn_change reveal={.property=KSN_SET_REVEAL,.value.reveal=it->reveal};
            r=ksn_view_change(view,tx,ref,&reveal);
        }
    }
    if(r==KSN_OK)r=ksn_view_submit(view,tx);
    if(r!=KSN_OK){(void)ksn_view_cancel(view,tx);return r;}
    *out=tx;return KSN_OK;
}
ksn_result ksn_presenter_patch(ksn_view *view,ksn_rect viewport,
                               const ksn_presenter_plan *plan,
                               const ksn_ref refs[KSN_PRESENTER_ITEMS],ksn_tx *out){
    if(!view||!plan||!refs||!out||!plan->patchable||plan->count>KSN_PRESENTER_ITEMS)
        return KSN_INVALID;
    ksn_tx tx;ksn_result r=ksn_view_begin(view,KSN_PATCH,&tx);
    if(r!=KSN_OK)return r;
    for(unsigned i=0;r==KSN_OK&&i<plan->count;i++){
        const ksn_presenter_item *it=&plan->items[i];
        ksn_rect bounds={it->bounds.x0+viewport.x0,it->bounds.y0+viewport.y0,
                         it->bounds.x1+viewport.x0,it->bounds.y1+viewport.y0};
        ksn_change rect_change={.property=KSN_SET_RECT,.value.rect=bounds};
        r=ksn_view_change(view,tx,refs[i],&rect_change);
        if(r!=KSN_OK)break;
        if(it->kind==KSN_IMAGE){
            ksn_change frame={.property=KSN_SET_IMAGE_FRAME,
                              .value.image={it->variant,it->frame}};
            r=ksn_view_change(view,tx,refs[i],&frame);
        }else{
            ksn_change color={.property=KSN_SET_COLOR,.value.color=it->color};
            r=ksn_view_change(view,tx,refs[i],&color);
            if(r==KSN_OK&&it->kind==KSN_TEXT&&it->text_id<KSN_PRESENTER_SLOTS){
                unsigned bytes;const char *s=text_for(plan,it->text_id,&bytes);
                ksn_change value={.property=KSN_SET_TEXT,
                                  .value.text={.utf8=s,.bytes=(uint16_t)bytes}};
                r=ksn_view_change(view,tx,refs[i],&value);
                if(r==KSN_OK&&it->reveal!=UINT8_MAX){
                    ksn_change reveal={.property=KSN_SET_REVEAL,.value.reveal=it->reveal};
                    r=ksn_view_change(view,tx,refs[i],&reveal);
                }
            }
        }
    }
    if(r==KSN_OK)r=ksn_view_submit(view,tx);
    if(r!=KSN_OK){(void)ksn_view_cancel(view,tx);return r;}
    *out=tx;return KSN_OK;
}
