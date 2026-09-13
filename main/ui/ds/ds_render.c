#include "ds_render.h"
#include <string.h>

/* The type is the format tag: these channels are premultiplied, never straight. */
typedef struct { uint8_t r,g,b,a; } ds_premultiplied_rgba8;
static unsigned mul8(unsigned a,unsigned b){return (a*b+127)/255;}
static unsigned clamp8(unsigned value){return value>255?255:value;}
static bool contains(const ds_frame_command *c,int x,int y){
    const ds_draw *d=&c->draw;
    return c->visible&&x>=d->bounds.x0&&x<d->bounds.x1&&y>=d->bounds.y0&&y<d->bounds.y1&&
           x>=d->clip.x0&&x<d->clip.x1&&y>=d->clip.y0&&y<d->clip.y1;
}
static void premultiply_over(ds_premultiplied_rgba8 *dst,ds_rgba color,uint8_t opacity){
    unsigned a=mul8(color&255,opacity),inverse=255-a;
    dst->r=(uint8_t)clamp8(mul8(color>>24,a)+mul8(dst->r,inverse));
    dst->g=(uint8_t)clamp8(mul8((color>>16)&255,a)+mul8(dst->g,inverse));
    dst->b=(uint8_t)clamp8(mul8((color>>8)&255,a)+mul8(dst->b,inverse));
    dst->a=(uint8_t)clamp8(a+mul8(dst->a,inverse));
}
static uint16_t group_over(uint16_t dst,ds_premultiplied_rgba8 src,uint8_t opacity){
    unsigned inverse=255-mul8(src.a,opacity);
    unsigned r=dst>>11,g=(dst>>5)&63,b=dst&31;
    r=clamp8(mul8(src.r,opacity)+mul8((r<<3)|(r>>2),inverse));
    g=clamp8(mul8(src.g,opacity)+mul8((g<<2)|(g>>4),inverse));
    b=clamp8(mul8(src.b,opacity)+mul8((b<<3)|(b>>2),inverse));
    return (uint16_t)((r>>3)<<11|(g>>2)<<5|(b>>3));
}
static ds_result render_group(ds_core *core,ds_tx ticket,ds_layer layer,unsigned first,unsigned end,
                               uint8_t opacity,int y,int rows,uint16_t *pixels){
    if(!opacity)return DS_OK;
    ds_premultiplied_rgba8 tile[64]; /* 256 bytes; no full component surface. */
    ds_frame_command command;
    int left=240,right=0,top=y+rows,bottom=y;
    for(unsigned i=first;i<=end;i++){
        ds_result result=ds_core_read(core,ticket,false,layer,(uint16_t)i,&command);
        if(result!=DS_OK)return result;
        const ds_draw *d=&command.draw;
        if(!command.visible||!d->opacity||!(d->data.shape.color&255))continue;
        int x0=d->bounds.x0>d->clip.x0?d->bounds.x0:d->clip.x0;
        int x1=d->bounds.x1<d->clip.x1?d->bounds.x1:d->clip.x1;
        int y0=d->bounds.y0>d->clip.y0?d->bounds.y0:d->clip.y0;
        int y1=d->bounds.y1<d->clip.y1?d->bounds.y1:d->clip.y1;
        if(x0>=x1||y0>=y1)continue;
        if(x0<left)left=x0;
        if(x1>right)right=x1;
        if(y0<top)top=y0;
        if(y1>bottom)bottom=y1;
    }
    if(left<0)left=0;
    if(right>240)right=240;
    if(top<y)top=y;
    if(bottom>y+rows)bottom=y+rows;
    for(int py=top;py<bottom;py++)for(int x0=left;x0<right;x0+=64){
        int count=right-x0;if(count>64)count=64;
        memset(tile,0,sizeof(tile));
        for(unsigned i=first;i<=end;i++){
            ds_result result=ds_core_read(core,ticket,false,layer,(uint16_t)i,&command);
            if(result!=DS_OK)return result;
            for(int x=0;x<count;x++)if(contains(&command,x0+x,py))
                premultiply_over(&tile[x],command.draw.data.shape.color,command.draw.opacity);
        }
        for(int x=0;x<count;x++){
            unsigned index=(unsigned)((py-y)*240+x0+x);
            pixels[index]=group_over(pixels[index],tile[x],opacity);
        }
    }
    return DS_OK;
}

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
            if(command.group_begin){
                unsigned first=i;uint8_t opacity=command.group_opacity;
                while(!command.group_end){
                    if(++i>=frame.next[layer].commands){ds_core_failed(core,frame.ticket);return DS_INVALID;}
                    result=ds_core_read(core,frame.ticket,false,(ds_layer)layer,(uint16_t)i,&command);
                    if(result!=DS_OK){ds_core_failed(core,frame.ticket);return result;}
                }
                result=render_group(core,frame.ticket,(ds_layer)layer,first,i,opacity,y,rows,pixels);
                if(result!=DS_OK){ds_core_failed(core,frame.ticket);return result;}
                continue;
            }
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
