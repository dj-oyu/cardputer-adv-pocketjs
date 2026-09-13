#include "ds_render.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"render line %d: %s\n",__LINE__,#x);return 1;}} while(0)
static uint16_t panel[240*135],strip[240*8];
static unsigned transfers,strip_calls;
static int fail_y=-1;
static uint16_t *get_strip(void *ctx){(void)ctx;strip_calls++;return strip;}
static ds_result send_strip(void *ctx,uint16_t y,uint16_t rows,const uint16_t *pixels){
    (void)ctx;transfers++;
    /* Model a failed call that physically changed part of the panel. */
    memcpy(panel+y*240,pixels,rows*240*sizeof(uint16_t));
    return y==fail_y?DS_IO:DS_OK;
}
static uint16_t color565(uint32_t c){return (uint16_t)((c>>27)<<11|((c>>18)&63)<<5|((c>>11)&31));}
static uint16_t reference_pixel(int x,int y,ds_rect rect,uint32_t color){
    uint16_t pixel=color565(0x0b1727ff);
    if(x>=rect.x0&&x<rect.x1&&y>=rect.y0&&y<rect.y1)pixel=color565(color);
    /* SYSTEM rectangle over the APP, including partial alpha. */
    if(x>=20&&x<200&&y>=48&&y<80){
        unsigned r=(pixel>>11)&31,g=(pixel>>5)&63,b=pixel&31;
        r=r*8+r/4;g=g*4+g/16;b=b*8+b/4;
        r=(245*128+r*127+127)/255;g=(187*128+g*127+127)/255;b=(105*128+b*127+127)/255;
        pixel=(uint16_t)((r/8)*2048+(g/4)*32+b/8);
    }
    return pixel;
}
static int compare_panel(ds_rect rect,uint32_t color){
    for(int y=0;y<135;y++)for(int x=0;x<240;x++)
        if(panel[y*240+x]!=reference_pixel(x,y,rect,color))return 0;
    return 1;
}
int main(void){
    ds_core core;ds_core_init(&core);ds_client app=ds_core_client(&core,DS_APP),sys=ds_core_client(&core,DS_SYSTEM);
    ds_display_port display={NULL,get_strip,send_strip,240,135,8};ds_render_stats stats;
    ds_tx tx;ds_ref moving,overlay;
    ds_draw d={.kind=DS_RECT,.bounds={0,0,32,16},.clip={0,0,240,135},.opacity=255};
    d.data.shape.color=0x67dfc7ff;
    CHECK(app.ops->begin(app.ctx,DS_REPLACE,&tx)==DS_OK);
    CHECK(app.ops->background(app.ctx,tx,0x0b1727ff)==DS_OK);
    CHECK(app.ops->add(app.ctx,tx,&d,&moving)==DS_OK);CHECK(app.ops->end(app.ctx,tx)==DS_OK);
    CHECK(ds_render_rects(&core,&display,&stats)==DS_OK&&stats.transferred_bytes==64800);
    d.bounds=(ds_rect){20,48,200,80};d.data.shape.color=0xf5bb6980;
    CHECK(sys.ops->begin(sys.ctx,DS_REPLACE,&tx)==DS_OK);CHECK(sys.ops->add(sys.ctx,tx,&d,&overlay)==DS_OK);
    CHECK(sys.ops->end(sys.ctx,tx)==DS_OK);CHECK(ds_render_rects(&core,&display,&stats)==DS_OK);
    ds_rect rect={0,0,32,16};
    for(unsigned i=0;i<150;i++){
        rect=(ds_rect){(int16_t)((i*17)%280-20),(int16_t)((i*11)%155-10),0,0};
        rect.x1=(int16_t)(rect.x0+32);rect.y1=(int16_t)(rect.y0+16);
        CHECK(app.ops->begin(app.ctx,DS_PATCH,&tx)==DS_OK);
        ds_change change={.property=DS_SET_RECT,.value.rect=rect};
        CHECK(app.ops->change(app.ctx,tx,moving,&change)==DS_OK);CHECK(app.ops->end(app.ctx,tx)==DS_OK);
        CHECK(ds_render_rects(&core,&display,&stats)==DS_OK);
        CHECK(compare_panel(rect,0x67dfc7ff));
    }
    unsigned before=transfers,borrowed=strip_calls;
    CHECK(app.ops->begin(app.ctx,DS_PATCH,&tx)==DS_OK);CHECK(app.ops->end(app.ctx,tx)==DS_OK);
    CHECK(ds_render_rects(&core,&display,&stats)==DS_OK&&stats.bands==0&&stats.transferred_bytes==0);
    CHECK(transfers==before&&strip_calls==borrowed);
    /* Partial failure, discard, and retry of the old baseline repair the LCD. */
    CHECK(app.ops->begin(app.ctx,DS_PATCH,&tx)==DS_OK);
    CHECK(app.ops->background(app.ctx,tx,0xffffffff)==DS_OK);CHECK(app.ops->end(app.ctx,tx)==DS_OK);
    ds_frame frame;CHECK(ds_core_frame(&core,&frame)==DS_OK);
    fail_y=8;CHECK(ds_render_rects(&core,&display,&stats)==DS_IO);
    CHECK(ds_core_discard(&core,frame.ticket)==DS_OK);fail_y=-1;
    CHECK(app.ops->begin(app.ctx,DS_PATCH,&tx)==DS_OK);CHECK(app.ops->end(app.ctx,tx)==DS_OK);
    CHECK(ds_render_rects(&core,&display,&stats)==DS_OK&&stats.transferred_bytes==64800);
    CHECK(compare_panel(rect,0x67dfc7ff));
    /* Same-length text at the same arena offset must still dirty its band. */
    CHECK(app.ops->begin(app.ctx,DS_REPLACE,&tx)==DS_OK);CHECK(app.ops->background(app.ctx,tx,0x0b1727ff)==DS_OK);
    d.kind=DS_TEXT;d.bounds=(ds_rect){1,128,20,135};d.data.text.utf8="ab";d.data.text.bytes=2;
    d.data.text.capacity=8;d.data.text.font=DS_BODY;d.data.text.color=0xffffffff;
    CHECK(app.ops->add(app.ctx,tx,&d,&moving)==DS_OK);CHECK(app.ops->end(app.ctx,tx)==DS_OK);
    before=transfers;CHECK(ds_render_rects(&core,&display,&stats)==DS_UNSUPPORTED&&transfers==before);
    CHECK(ds_core_frame(&core,&frame)==DS_OK);CHECK(ds_core_presented(&core,frame.ticket)==DS_OK);
    CHECK(app.ops->begin(app.ctx,DS_PATCH,&tx)==DS_OK);
    ds_change text={.property=DS_SET_TEXT,.value.text={"cd",2}};
    CHECK(app.ops->change(app.ctx,tx,moving,&text)==DS_OK);CHECK(app.ops->end(app.ctx,tx)==DS_OK);
    CHECK(ds_core_frame(&core,&frame)==DS_OK);uint32_t mask;
    CHECK(ds_core_damage(&core,frame.ticket,&mask)==DS_OK&&mask==(1u<<16));
    CHECK(ds_core_discard(&core,frame.ticket)==DS_OK);
    CHECK(ds_core_damage(&core,frame.ticket,&mask)==DS_STALE);
    puts("damage and rectangle renderer: PASS");return 0;
}
