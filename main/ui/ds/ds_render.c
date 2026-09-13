#include "ds_render.h"

static uint16_t rgb565(ds_rgba c){return (uint16_t)((c>>27)<<11|((c>>18)&63)<<5|((c>>11)&31));}
static uint16_t blend(uint16_t dst,ds_rgba src,uint8_t opacity){
    unsigned a=((src&255)*opacity+127)/255;
    if(!a)return dst;
    unsigned r=(dst>>11)&31,g=(dst>>5)&63,b=dst&31;
    r=(r<<3)|(r>>2);g=(g<<2)|(g>>4);b=(b<<3)|(b>>2);
    r=((src>>24)*a+r*(255-a)+127)/255;
    g=(((src>>16)&255)*a+g*(255-a)+127)/255;
    b=(((src>>8)&255)*a+b*(255-a)+127)/255;
    return (uint16_t)((r>>3)<<11|(g>>2)<<5|(b>>3));
}
ds_result ds_render_rects(ds_core *core,const ds_display_port *display,ds_render_stats *stats){
    if(!core||!display||!stats||!display->strip||!display->present||
       display->width!=240||display->height!=135||display->strip_rows!=8)return DS_INVALID;
    *stats=(ds_render_stats){0};
    ds_frame frame;ds_result result=ds_core_frame(core,&frame);
    if(result!=DS_OK)return result;
    uint32_t mask;result=ds_core_damage(core,frame.ticket,&mask);
    if(result!=DS_OK)return result;
    if(!mask)return ds_core_presented(core,frame.ticket);
    ds_frame_command command;
    for(unsigned layer=0;layer<2;layer++)for(unsigned i=0;i<frame.next[layer].commands;i++){
        result=ds_core_read(core,frame.ticket,false,(ds_layer)layer,i,&command);
        if(result!=DS_OK)return result;
        if(command.draw.kind!=DS_RECT)return DS_UNSUPPORTED;
    }
    uint16_t *pixels=display->strip(display->ctx);
    if(!pixels)return DS_OOM;
    for(unsigned band=0;band<17;band++){
        if(!(mask&(1u<<band)))continue;
        int y=(int)band*8,rows=band==16?7:8;
        for(int i=0;i<240*rows;i++)pixels[i]=rgb565(frame.next_background);
        for(unsigned layer=0;layer<2;layer++)for(unsigned i=0;i<frame.next[layer].commands;i++){
            result=ds_core_read(core,frame.ticket,false,(ds_layer)layer,i,&command);
            if(result!=DS_OK){ds_core_failed(core,frame.ticket);return result;}
            ds_draw *d=&command.draw;
            if(!command.visible||!d->opacity)continue;
            int x0=d->bounds.x0,x1=d->bounds.x1,y0=d->bounds.y0,y1=d->bounds.y1;
            if(x0<d->clip.x0)x0=d->clip.x0;
            if(x1>d->clip.x1)x1=d->clip.x1;
            if(y0<d->clip.y0)y0=d->clip.y0;
            if(y1>d->clip.y1)y1=d->clip.y1;
            if(x0<0)x0=0;
            if(x1>240)x1=240;
            if(y0<y)y0=y;
            if(y1>y+rows)y1=y+rows;
            for(int py=y0;py<y1;py++)for(int x=x0;x<x1;x++){
                unsigned index=(unsigned)((py-y)*240+x);
                pixels[index]=blend(pixels[index],d->data.shape.color,d->opacity);
            }
        }
        result=display->present(display->ctx,(uint16_t)y,(uint16_t)rows,pixels);
        if(result!=DS_OK){ds_core_failed(core,frame.ticket);return result;}
        stats->bands|=1u<<band;stats->transferred_bytes+=(uint32_t)rows*240u*2u;
    }
    return ds_core_presented(core,frame.ticket);
}
