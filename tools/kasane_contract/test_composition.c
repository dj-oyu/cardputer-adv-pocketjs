#include "ksn_cache.h"
#include "ksn_modal.h"
#include "ksn_render.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"composition line %d: %s\n",__LINE__,#x);return 1;}} while(0)
static uint16_t panel[240*135],strip[240*8];
static int fail_y=-1;
static uint16_t *get_strip(void *ctx){(void)ctx;return strip;}
static ksn_result send_strip(void *ctx,uint16_t y,uint16_t rows,const uint16_t *pixels){
    (void)ctx;memcpy(panel+y*240,pixels,rows*240*sizeof(*pixels));return y==fail_y?KSN_IO:KSN_OK;
}
static unsigned m(unsigned a,unsigned b){return (a*b+127)/255;}
static unsigned sat(unsigned a){return a>255?255:a;}
/* Independent full-panel scalar reference; no renderer helpers or bank reads. */
static uint16_t reference(unsigned opacity,int x,int y){
    unsigned p[4]={0};
    const unsigned colors[2][4]={{255,31,7,179},{13,199,251,237}};
    const unsigned command_opacity[2]={211,193};
    const ksn_rect boxes[2]={{6,5,74,29},{31,13,98,42}};
    for(unsigned i=0;i<2;i++)if(x>=boxes[i].x0&&x<boxes[i].x1&&y>=boxes[i].y0&&y<boxes[i].y1){
        unsigned a=m(colors[i][3],command_opacity[i]);
        for(unsigned c=0;c<3;c++)p[c]=sat(m(colors[i][c],a)+m(p[c],255-a));
        p[3]=sat(a+m(p[3],255-a));
    }
    unsigned a=m(p[3],opacity),bg[3]={24,65,107}; /* RGB565 bit-expanded background. */
    unsigned c[3];for(unsigned i=0;i<3;i++)c[i]=sat(m(p[i],opacity)+m(bg[i],255-a));
    return (uint16_t)((c[0]>>3)<<11|(c[1]>>2)<<5|(c[2]>>3));
}
static int groups(void){
    ksn_core core;ksn_cache cache;ksn_cache_command_block commands;ksn_cache_text_block text;
    ksn_core_init(&core);CHECK(ksn_cache_bind(&cache,&commands,&text)==KSN_OK);
    ksn_client app=ksn_core_client(&core,KSN_APP);ksn_tx tx;
    ksn_draw draw[2]={
        {.kind=KSN_RECT,.bounds={6,5,74,29},.clip={0,0,240,135},.opacity=211,.data.shape={0xff1f07b3,0,0}},
        {.kind=KSN_RECT,.bounds={31,13,98,42},.clip={0,0,240,135},.opacity=193,.data.shape={0x0dc7fbed,0,0}}
    };
    ksn_template t;ksn_instance instance;ksn_placement place={0,0,{0,0,240,135},255,true};
    CHECK(ksn_cache_create(&cache,KSN_APP,draw,2,&t)==KSN_OK);
    CHECK(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    CHECK(app.ops->background(app.ctx,tx,0x18416bff)==KSN_OK);
    CHECK(ksn_cache_instantiate(&cache,&core,tx,t,&place,&instance)==KSN_OK);
    CHECK(app.ops->end(app.ctx,tx)==KSN_OK);
    ksn_display_port display={NULL,get_strip,send_strip,240,135,8};ksn_render_stats stats;
    CHECK(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    CHECK(ksn_cache_resolve(&cache,&core,tx,true)==KSN_OK);
    /* Exhaust every group opacity, including 0/1/127/128/254/255 and tile edges. */
    for(unsigned opacity=0;opacity<256;opacity++){
        CHECK(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);place.opacity=(uint8_t)opacity;
        CHECK(ksn_cache_place(&cache,&core,tx,instance,&place)==KSN_OK);
        CHECK(app.ops->end(app.ctx,tx)==KSN_OK);
        CHECK(ksn_cache_resolve(&cache,&core,tx,true)==KSN_BUSY);
        CHECK(ksn_render_rects(&core,&display,&stats)==KSN_OK);
        CHECK(ksn_cache_resolve(&cache,&core,(ksn_tx){tx.value-1},true)==KSN_STALE);
        CHECK(ksn_cache_resolve(&cache,&core,tx,false)==KSN_STALE);
        CHECK(ksn_cache_resolve(&cache,&core,tx,true)==KSN_OK);
        for(int y=0;y<135;y++)for(int x=0;x<240;x++)CHECK(panel[y*240+x]==reference(opacity,x,y));
    }
    CHECK(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
    CHECK(ksn_cache_place(&cache,&core,tx,instance,&place)==KSN_OK);
    CHECK(app.ops->end(app.ctx,tx)==KSN_OK);CHECK(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    CHECK(stats.bands==0);CHECK(ksn_cache_resolve(&cache,&core,tx,true)==KSN_OK);

    /* An opacity update that touched the LCD can be discarded and repaired. */
    CHECK(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);place.opacity=1;
    CHECK(ksn_cache_place(&cache,&core,tx,instance,&place)==KSN_OK);
    CHECK(app.ops->end(app.ctx,tx)==KSN_OK);fail_y=8;
    CHECK(ksn_render_rects(&core,&display,&stats)==KSN_IO);fail_y=-1;
    CHECK(ksn_core_discard_reason(&core,tx,KSN_CANCELLED)==KSN_OK);
    CHECK(ksn_cache_resolve(&cache,&core,tx,false)==KSN_OK);
    CHECK(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);CHECK(app.ops->end(app.ctx,tx)==KSN_OK);
    CHECK(ksn_render_rects(&core,&display,&stats)==KSN_OK&&stats.transferred_bytes==64800);
    for(int y=0;y<135;y++)for(int x=0;x<240;x++)CHECK(panel[y*240+x]==reference(255,x,y));

    /* Group membership cannot be partially overlapped or nested. */
    CHECK(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);ksn_ref first,second;
    CHECK(app.ops->add(app.ctx,tx,&draw[0],&first)==KSN_OK);
    CHECK(app.ops->add(app.ctx,tx,&draw[1],&second)==KSN_OK);
    CHECK(ksn_core_group(&core,KSN_APP,tx,first,2,128)==KSN_OK);
    CHECK(ksn_core_group(&core,KSN_APP,tx,second,1,128)==KSN_INVALID);
    CHECK(app.ops->end(app.ctx,tx)==KSN_INVALID);app.ops->abort(app.ctx,tx);
    return 0;
}
static int modals(void){
    ksn_core core;ksn_core_init(&core);ksn_modal modal;ksn_modal_init(&modal,42);
    ksn_client app=ksn_core_client(&core,KSN_APP),system=ksn_core_client(&core,KSN_SYSTEM);ksn_tx tx;
    ksn_display_port display={NULL,get_strip,send_strip,240,135,8};ksn_render_stats stats;
    CHECK(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    CHECK(app.ops->background(app.ctx,tx,0xffffffff)==KSN_OK);CHECK(app.ops->end(app.ctx,tx)==KSN_OK);
    CHECK(ksn_core_poll(&core).status==KSN_SUBMITTED);
    CHECK(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    CHECK(ksn_modal_route(&modal,&core,false)==KSN_INPUT_APP);
    ksn_submission old=ksn_core_poll(&core);
    CHECK(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    CHECK(ksn_modal_prepare_open(&modal,&core,tx,KSN_MODAL_DIM_LIVE,0x00000080,7)==KSN_OK);
    CHECK(ksn_modal_route(&modal,&core,false)==KSN_INPUT_BLOCKED);
    CHECK(ksn_modal_route(&modal,&core,true)==KSN_INPUT_HOST);
    CHECK(ksn_modal_resolve(&modal,&core)==KSN_STALE);
    CHECK(ksn_core_poll(&core).ticket.value==old.ticket.value);
    CHECK(ksn_modal_cancel(&modal,&core)==KSN_OK);CHECK(modal.phase==KSN_MODAL_CLOSED&&modal.focus==42);

    CHECK(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    CHECK(app.ops->background(app.ctx,tx,0xffffffff)==KSN_OK);
    CHECK(ksn_modal_prepare_open(&modal,&core,tx,KSN_MODAL_DIM_LIVE,0x00000080,7)==KSN_OK);
    CHECK(app.ops->end(app.ctx,tx)==KSN_OK);CHECK(ksn_modal_resolve(&modal,&core)==KSN_BUSY);
    fail_y=8;CHECK(ksn_render_rects(&core,&display,&stats)==KSN_IO);
    CHECK(ksn_core_poll(&core).status==KSN_SUBMITTED&&ksn_core_poll(&core).reason==KSN_IO);
    CHECK(ksn_modal_route(&modal,&core,false)==KSN_INPUT_BLOCKED);
    fail_y=-1;CHECK(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    CHECK(stats.transferred_bytes==64800);CHECK(ksn_modal_resolve(&modal,&core)==KSN_OK);
    CHECK(modal.phase==KSN_MODAL_OPEN&&modal.focus==7);
    CHECK(ksn_modal_route(&modal,&core,false)==KSN_INPUT_MODAL);
    CHECK(panel[0]==0x7bef); /* white under a black alpha-128 scrim */
    /* The live background can update while focus remains in the modal. */
    CHECK(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
    CHECK(app.ops->background(app.ctx,tx,0x00ff00ff)==KSN_OK);CHECK(app.ops->end(app.ctx,tx)==KSN_OK);
    CHECK(ksn_render_rects(&core,&display,&stats)==KSN_OK&&panel[0]==0x03e0);
    CHECK(ksn_modal_route(&modal,&core,false)==KSN_INPUT_MODAL&&modal.focus==7);
    CHECK(ksn_modal_prepare_open(&modal,&core,tx,KSN_MODAL_SOLID,0x000000ff,9)==KSN_BUSY);

    /* SYSTEM remains above the modal; its result cannot switch APP scope. */
    CHECK(system.ops->begin(system.ctx,KSN_REPLACE,&tx)==KSN_OK);
    ksn_draw notification={.kind=KSN_RECT,.bounds={0,0,240,8},.clip={0,0,240,135},.opacity=255,.data.shape={0xff0000ff,0,0}};
    ksn_ref ref;CHECK(system.ops->add(system.ctx,tx,&notification,&ref)==KSN_OK);
    CHECK(system.ops->end(system.ctx,tx)==KSN_OK);CHECK(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    CHECK(panel[0]==0xf800);CHECK(ksn_modal_resolve(&modal,&core)==KSN_STALE);

    /* Failed closing keeps the modal and suppresses input until repair. */
    CHECK(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    CHECK(ksn_modal_prepare_close(&modal,&core,tx)==KSN_OK);
    CHECK(app.ops->background(app.ctx,tx,0x0000ffff)==KSN_OK);CHECK(app.ops->end(app.ctx,tx)==KSN_OK);
    fail_y=16;CHECK(ksn_render_rects(&core,&display,&stats)==KSN_IO);fail_y=-1;
    CHECK(ksn_modal_cancel(&modal,&core)==KSN_OK);CHECK(modal.phase==KSN_MODAL_OPEN);
    CHECK(ksn_core_poll(&core).reason==KSN_CANCELLED);
    CHECK(ksn_modal_route(&modal,&core,false)==KSN_INPUT_BLOCKED);
    CHECK(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);CHECK(app.ops->end(app.ctx,tx)==KSN_OK);
    CHECK(ksn_render_rects(&core,&display,&stats)==KSN_OK&&stats.transferred_bytes==64800);
    CHECK(ksn_modal_route(&modal,&core,false)==KSN_INPUT_MODAL);

    CHECK(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    CHECK(ksn_modal_prepare_close(&modal,&core,tx)==KSN_OK);
    CHECK(app.ops->background(app.ctx,tx,0x0000ffff)==KSN_OK);CHECK(app.ops->end(app.ctx,tx)==KSN_OK);
    CHECK(ksn_render_rects(&core,&display,&stats)==KSN_OK);CHECK(ksn_modal_resolve(&modal,&core)==KSN_OK);
    CHECK(modal.phase==KSN_MODAL_CLOSED&&modal.focus==42);CHECK(panel[240*20]==0x001f);
    const uint32_t keys[]={17,23};CHECK(ksn_modal_focus(&modal,keys,2)==KSN_OK&&modal.focus==17);
    CHECK(ksn_modal_focus(&modal,NULL,0)==KSN_OK&&modal.focus==0);

    CHECK(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    CHECK(ksn_modal_prepare_open(&modal,&core,tx,KSN_MODAL_SOLID,0x12345680,2)==KSN_INVALID);
    CHECK(ksn_modal_prepare_open(&modal,&core,tx,KSN_MODAL_SOLID,0x123456ff,2)==KSN_OK);
    CHECK(app.ops->end(app.ctx,tx)==KSN_OK);CHECK(ksn_modal_cancel(&modal,&core)==KSN_OK);
    CHECK(modal.phase==KSN_MODAL_CLOSED);

    /* Quota failure after preparing never installs the candidate input scope. */
    CHECK(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    CHECK(ksn_modal_prepare_open(&modal,&core,tx,KSN_MODAL_DIM_LIVE,0x00000080,8)==KSN_OK);
    for(unsigned i=1;i<KSN_APP_COMMANDS;i++)CHECK(app.ops->add(app.ctx,tx,&notification,&ref)==KSN_OK);
    CHECK(app.ops->add(app.ctx,tx,&notification,&ref)==KSN_LIMIT);
    CHECK(app.ops->end(app.ctx,tx)==KSN_LIMIT);
    CHECK(ksn_modal_route(&modal,&core,false)==KSN_INPUT_BLOCKED);
    CHECK(ksn_modal_cancel(&modal,&core)==KSN_OK&&modal.phase==KSN_MODAL_CLOSED);
    CHECK(ksn_modal_route(&modal,&core,false)==KSN_INPUT_APP);
    CHECK(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    CHECK(ksn_modal_prepare_open(&modal,&core,tx,KSN_MODAL_SOLID,0x00ff00ff,9)==KSN_OK);
    CHECK(app.ops->end(app.ctx,tx)==KSN_OK);
    CHECK(ksn_core_submission_usage(&core,KSN_APP).commands==0);
    CHECK(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    CHECK(ksn_modal_resolve(&modal,&core)==KSN_OK&&modal.focus==9);
    CHECK(panel[240*20]==0x07e0&&panel[0]==0xf800);
    return 0;
}
int main(void){CHECK(groups()==0);CHECK(modals()==0);puts("isolated groups and modal lifecycle: PASS");return 0;}
