#include "core_fixture.h"
#include "ksn_render.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"render line %d: %s\n",__LINE__,#x);return 1;}} while(0)
static uint16_t panel[240*135],strip[240*8];
static unsigned transfers,strip_calls;
static unsigned backdrop_loads;
static int backdrop_fail_y=-1;
static int fail_y=-1;
static uint16_t *get_strip(void *ctx){(void)ctx;strip_calls++;return strip;}
static ksn_result send_strip(void *ctx,uint16_t y,uint16_t rows,const uint16_t *pixels){
    (void)ctx;transfers++;
    /* Model a failed call that physically changed part of the panel. */
    memcpy(panel+y*240,pixels,rows*240*sizeof(uint16_t));
    return y==fail_y?KSN_IO:KSN_OK;
}
static ksn_result load_backdrop(void *ctx,uint16_t y,uint16_t rows,uint16_t *pixels){
    (void)ctx;backdrop_loads++;
    if(y==backdrop_fail_y)return KSN_IO;
    for(unsigned i=0;i<(unsigned)rows*240;i++)pixels[i]=0x07e0;
    return KSN_OK;
}
static uint16_t color565(uint32_t c){return (uint16_t)((c>>27)<<11|((c>>18)&63)<<5|((c>>11)&31));}
static uint16_t reference_pixel(int x,int y,ksn_rect rect,uint32_t color){
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
static int compare_panel(ksn_rect rect,uint32_t color){
    for(int y=0;y<135;y++)for(int x=0;x<240;x++)
        if(panel[y*240+x]!=reference_pixel(x,y,rect,color))return 0;
    return 1;
}
int main(void){
    /* A host-provided scene replaces only the APP background fill. Commands
     * still composite normally, and every invalidated band is loaded once. */
    {
        KSN_TEST_CORE(backdrop,);ksn_core_init(&backdrop);
        ksn_client client=ksn_core_client(&backdrop,KSN_APP);ksn_tx bt;ksn_ref br;
        ksn_draw box={.kind=KSN_RECT,.bounds={2,2,6,6},.clip={0,0,240,135},
                      .opacity=255,.data.shape.color=0xf80000ff};
        CHECK(client.ops->begin(client.ctx,KSN_REPLACE,&bt)==KSN_OK);
        CHECK(client.ops->background(client.ctx,bt,0x000000ff)==KSN_OK);
        CHECK(client.ops->add(client.ctx,bt,&box,&br)==KSN_OK);
        CHECK(client.ops->end(client.ctx,bt)==KSN_OK);
        memset(panel,0,sizeof(panel));backdrop_loads=0;
        ksn_display_port port={.strip=get_strip,.present=send_strip,.width=240,.height=135,
            .strip_rows=8};
        ksn_render_stats rendered;
        CHECK(ksn_render_rects_backdrop(&backdrop,&port,load_backdrop,false,&rendered)==KSN_OK);
        CHECK(backdrop_loads==17&&panel[0]==0x07e0&&panel[2+2*240]==0xf800);
        ksn_core_invalidate(&backdrop);backdrop_fail_y=8;
        CHECK(ksn_render_rects_backdrop(&backdrop,&port,load_backdrop,false,&rendered)==KSN_IO);
        CHECK(ksn_core_needs_repair(&backdrop));
        backdrop_fail_y=-1;backdrop_loads=0;
        CHECK(ksn_render_rects_backdrop(&backdrop,&port,load_backdrop,false,&rendered)==KSN_OK);
        CHECK(backdrop_loads==17&&!ksn_core_needs_repair(&backdrop));
    }
    /* Opaque SYSTEM rows hide APP and scene damage only while the SYSTEM
     * pixels are committed. A pending clear must repaint the exposed scene. */
    {
        KSN_TEST_CORE(occlusion,);ksn_core_init(&occlusion);
        ksn_client app=ksn_core_client(&occlusion,KSN_APP);
        ksn_client sys=ksn_core_client(&occlusion,KSN_SYSTEM);
        ksn_display_port port={.strip=get_strip,.present=send_strip,.width=240,
                               .height=135,.strip_rows=8};
        ksn_render_stats rendered;ksn_tx tx;ksn_ref ref;
        ksn_draw opaque={.kind=KSN_RECT,.bounds={0,0,240,48},
                         .clip={0,0,240,48},.opacity=255,
                         .data.shape={.color=0x102030ff}};
        CHECK(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
        CHECK(app.ops->background(app.ctx,tx,0x000000ff)==KSN_OK);
        ksn_draw behind={.kind=KSN_RECT,.bounds={0,0,240,48},
                         .clip={0,0,240,48},.opacity=255,
                         .data.shape={.color=0xff0000ff}};
        CHECK(app.ops->add(app.ctx,tx,&behind,&ref)==KSN_OK);
        CHECK(app.ops->end(app.ctx,tx)==KSN_OK);
        CHECK(ksn_render_rects_backdrop(&occlusion,&port,load_backdrop,true,&rendered)==KSN_OK);
        CHECK(sys.ops->begin(sys.ctx,KSN_REPLACE,&tx)==KSN_OK);
        CHECK(sys.ops->add(sys.ctx,tx,&opaque,&ref)==KSN_OK);
        CHECK(sys.ops->end(sys.ctx,tx)==KSN_OK);
        backdrop_loads=0;
        CHECK(ksn_render_rects_backdrop(&occlusion,&port,load_backdrop,true,&rendered)==KSN_OK);
        CHECK(backdrop_loads==11&&rendered.bands==KSN_BANDS_ALL);
        uint16_t covered_pixel=panel[0];
        CHECK(covered_pixel==color565(0x102030ff));
        CHECK(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
        CHECK(app.ops->background(app.ctx,tx,0xffffffff)==KSN_OK);
        CHECK(app.ops->end(app.ctx,tx)==KSN_OK);
        backdrop_loads=0;ksn_core_invalidate(&occlusion);
        CHECK(ksn_render_rects_backdrop(&occlusion,&port,load_backdrop,true,&rendered)==KSN_OK);
        CHECK((rendered.bands&0x3fu)==0&&backdrop_loads==11&&panel[0]==covered_pixel);
        CHECK(sys.ops->begin(sys.ctx,KSN_REPLACE,&tx)==KSN_OK);
        CHECK(sys.ops->end(sys.ctx,tx)==KSN_OK);
        CHECK(ksn_render_rects_backdrop(&occlusion,&port,load_backdrop,true,&rendered)==KSN_OK);
        CHECK((rendered.bands&0x3fu)==0x3fu&&panel[0]==color565(0xff0000ff));
    }
    KSN_TEST_CORE(core,);ksn_core_init(&core);ksn_client app=ksn_core_client(&core,KSN_APP),sys=ksn_core_client(&core,KSN_SYSTEM);
    ksn_display_port display={NULL,get_strip,send_strip,240,135,8,NULL,NULL};ksn_render_stats stats;
    ksn_tx tx;ksn_ref moving,overlay;
    ksn_draw d={.kind=KSN_RECT,.bounds={0,0,32,16},.clip={0,0,240,135},.opacity=255};
    d.data.shape.color=0x67dfc7ff;
    CHECK(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    CHECK(app.ops->background(app.ctx,tx,0x0b1727ff)==KSN_OK);
    CHECK(app.ops->add(app.ctx,tx,&d,&moving)==KSN_OK);CHECK(app.ops->end(app.ctx,tx)==KSN_OK);
    CHECK(ksn_render_rects(&core,&display,&stats)==KSN_OK&&stats.transferred_bytes==64800);
    d.bounds=(ksn_rect){20,48,200,80};d.data.shape.color=0xf5bb6980;
    CHECK(sys.ops->begin(sys.ctx,KSN_REPLACE,&tx)==KSN_OK);CHECK(sys.ops->add(sys.ctx,tx,&d,&overlay)==KSN_OK);
    CHECK(sys.ops->end(sys.ctx,tx)==KSN_OK);CHECK(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    ksn_rect rect={0,0,32,16};
    for(unsigned i=0;i<150;i++){
        rect=(ksn_rect){(int16_t)((i*17)%280-20),(int16_t)((i*11)%155-10),0,0};
        rect.x1=(int16_t)(rect.x0+32);rect.y1=(int16_t)(rect.y0+16);
        CHECK(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
        ksn_change change={.property=KSN_SET_RECT,.value.rect=rect};
        CHECK(app.ops->change(app.ctx,tx,moving,&change)==KSN_OK);CHECK(app.ops->end(app.ctx,tx)==KSN_OK);
        CHECK(ksn_render_rects(&core,&display,&stats)==KSN_OK);
        CHECK(compare_panel(rect,0x67dfc7ff));
    }
    unsigned before=transfers,borrowed=strip_calls;
    CHECK(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);CHECK(app.ops->end(app.ctx,tx)==KSN_OK);
    CHECK(ksn_render_rects(&core,&display,&stats)==KSN_OK&&stats.bands==0&&stats.transferred_bytes==0);
    CHECK(transfers==before&&strip_calls==borrowed);
    /* Partial failure, discard, and retry of the old baseline repair the LCD. */
    CHECK(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
    CHECK(app.ops->background(app.ctx,tx,0xffffffff)==KSN_OK);CHECK(app.ops->end(app.ctx,tx)==KSN_OK);
    ksn_frame frame;CHECK(ksn_core_frame(&core,&frame)==KSN_OK);
    fail_y=8;CHECK(ksn_render_rects(&core,&display,&stats)==KSN_IO);
    CHECK(ksn_core_discard(&core,frame.ticket)==KSN_OK);fail_y=-1;
    CHECK(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);CHECK(app.ops->end(app.ctx,tx)==KSN_OK);
    CHECK(ksn_render_rects(&core,&display,&stats)==KSN_OK&&stats.transferred_bytes==64800);
    CHECK(compare_panel(rect,0x67dfc7ff));
    /* Same-length text at the same arena offset must still dirty its band. */
    CHECK(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);CHECK(app.ops->background(app.ctx,tx,0x0b1727ff)==KSN_OK);
    d.kind=KSN_TEXT;d.bounds=(ksn_rect){1,128,20,135};d.data.text.utf8="ab";d.data.text.bytes=2;
    d.data.text.capacity=8;d.data.text.font=KSN_BODY;d.data.text.color=0xffffffff;
    CHECK(app.ops->add(app.ctx,tx,&d,&moving)==KSN_OK);CHECK(app.ops->end(app.ctx,tx)==KSN_OK);
    before=transfers;CHECK(ksn_render_rects(&core,&display,&stats)==KSN_UNSUPPORTED&&transfers==before);
    CHECK(ksn_core_frame(&core,&frame)==KSN_OK);CHECK(ksn_core_presented(&core,frame.ticket)==KSN_OK);
    CHECK(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
    ksn_change text={.property=KSN_SET_TEXT,.value.text={"cd",2}};
    CHECK(app.ops->change(app.ctx,tx,moving,&text)==KSN_OK);CHECK(app.ops->end(app.ctx,tx)==KSN_OK);
    CHECK(ksn_core_frame(&core,&frame)==KSN_OK);ksn_damage damage;
    CHECK(ksn_core_damage(&core,frame.ticket,NULL,&damage)==KSN_OK&&damage.bands==(1u<<16));
    /* The text moved inside one band, so the band's columns are the union of
     * where it was and where it is -- not the whole width. */
    CHECK(damage.x0[16]>=0&&damage.x1[16]<=240&&damage.x1[16]-damage.x0[16]<240);
    CHECK(ksn_core_discard(&core,frame.ticket)==KSN_OK);
    CHECK(ksn_core_damage(&core,frame.ticket,NULL,&damage)==KSN_STALE);
    puts("damage and rectangle renderer: PASS");return 0;
}
