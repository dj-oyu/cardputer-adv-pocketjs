#include "ds_view_host.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"view line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static ds_core core;
static ds_cache cache;
static uint16_t strip[240*8],panel[240*135];
static int fail_y=-1;
static uint16_t *buffer(void *ctx){(void)ctx;return strip;}
static ds_result transfer(void *ctx,uint16_t y,uint16_t rows,const uint16_t *p){
    (void)ctx;memcpy(panel+y*240,p,rows*240*sizeof(*p));return y==fail_y?DS_IO:DS_OK;
}
int main(void){
    ds_view_host host;ds_view_host_init(&host,&core,&cache,42);
    ds_view *app=ds_view_host_endpoint(&host,DS_APP),*sys=ds_view_host_endpoint(&host,DS_SYSTEM);
    ds_display_port port={NULL,buffer,transfer,240,135,8};ds_render_stats stats;
    ds_draw d={.kind=DS_RECT,.bounds={0,0,16,16},.clip={0,0,240,135},.opacity=255,.data.shape={0xff0000ff,0,0}};
    ds_placement p={8,8,{0,0,240,135},128,true};
    ds_template t;ds_instance a,b;ds_tx tx,other;ds_ref ref;
    CHECK(ds_view_features(app).modal&&!ds_view_features(sys).modal);
    CHECK(ds_view_features(app).draw_kinds==(1u<<DS_RECT));
    CHECK(!ds_view_features(app).animation&&!ds_view_features(app).frosted);
    CHECK(ds_view_cache_create(app,&d,1,&t)==DS_OK);
    CHECK(ds_view_cache_release(sys,t)==DS_STALE);
    CHECK(ds_view_begin(app,DS_REPLACE,&tx)==DS_OK);
    CHECK(ds_view_background(app,tx,0x000000ff)==DS_OK);
    CHECK(ds_view_instantiate(app,tx,t,&p,&a)==DS_OK);
    p.x=32;CHECK(ds_view_instantiate(app,tx,t,&p,&b)==DS_OK);
    CHECK(ds_view_cancel(sys,tx)==DS_STALE);
    CHECK(ds_view_visible(sys,tx,a,false)==DS_STALE);
    CHECK(ds_view_begin(sys,DS_PATCH,&other)==DS_BUSY);
    CHECK(ds_view_submit(app,tx)==DS_OK);
    ds_view_host_end_turn(&host); /* Submitted work must survive return/yield. */
    CHECK(ds_view_poll(app).status==DS_SUBMITTED);
    CHECK(ds_view_host_present(&host,&port,&stats)==DS_OK);
    CHECK(ds_view_poll(app).status==DS_PRESENTED);
    CHECK(panel[8*240+8]==0x8000&&panel[8*240+32]==0x8000);
    CHECK(ds_view_cache_release(app,t)==DS_BUSY);
    ds_tx app_ticket=tx;
    CHECK(ds_view_begin(sys,DS_PATCH,&tx)==DS_OK);
    CHECK(ds_view_place(sys,tx,a,&p)==DS_STALE);
    CHECK(ds_view_begin(sys,DS_REPLACE,&tx)==DS_OK);
    CHECK(ds_view_instantiate(sys,tx,t,&p,&b)==DS_STALE);
    CHECK(ds_view_begin(sys,DS_REPLACE,&tx)==DS_OK);
    CHECK(ds_view_submit(sys,tx)==DS_OK);
    CHECK(ds_view_host_present(&host,&port,&stats)==DS_OK);
    CHECK(ds_view_poll(app).ticket.value==app_ticket.value);
    CHECK(ds_view_poll(sys).ticket.value==tx.value);
    /* PATCH rollback restores placement/visibility, then next update works. */
    CHECK(ds_view_begin(app,DS_PATCH,&tx)==DS_OK);
    CHECK(ds_view_visible(app,tx,a,false)==DS_OK);
    CHECK(ds_view_cancel(app,tx)==DS_OK);
    CHECK(ds_view_begin(app,DS_PATCH,&tx)==DS_OK);
    CHECK(ds_view_submit(app,tx)==DS_OK);
    CHECK(ds_view_host_present(&host,&port,&stats)==DS_OK&&stats.bands==0);
    /* An unfinished modal and new cache instance are both cleaned on yield. */
    CHECK(ds_view_begin(app,DS_REPLACE,&tx)==DS_OK);
    CHECK(ds_view_modal_open(app,tx,DS_MODAL_SOLID,0x001122ff,7)==DS_OK);
    CHECK(ds_view_instantiate(app,tx,t,&p,&b)==DS_OK);
    ds_view_host_end_turn(&host);
    CHECK(ds_view_host_route(&host,false)==DS_INPUT_APP);
    CHECK(ds_cache_get_stats(&cache).instances==2);
    CHECK(ds_view_begin(app,DS_REPLACE,&tx)==DS_OK);
    CHECK(ds_view_modal_open(app,tx,DS_MODAL_SOLID,0x001122ff,7)==DS_OK);
    CHECK(ds_view_submit(app,tx)==DS_OK);
    fail_y=8;
    CHECK(ds_view_host_present(&host,&port,&stats)==DS_IO);
    CHECK(ds_view_poll(app).status==DS_SUBMITTED);
    CHECK(ds_view_host_route(&host,false)==DS_INPUT_BLOCKED);
    CHECK(ds_view_host_route(&host,true)==DS_INPUT_HOST);
    CHECK(ds_view_cancel(app,tx)==DS_OK);
    CHECK(ds_view_poll(app).status==DS_DISCARDED&&ds_view_poll(app).reason==DS_CANCELLED);
    CHECK(ds_view_host_route(&host,false)==DS_INPUT_BLOCKED);
    CHECK(ds_view_begin(app,DS_PATCH,&tx)==DS_OK);
    CHECK(ds_view_submit(app,tx)==DS_OK);fail_y=-1;
    CHECK(ds_view_host_present(&host,&port,&stats)==DS_OK&&stats.transferred_bytes==64800);
    CHECK(ds_view_host_route(&host,false)==DS_INPUT_APP);
    CHECK(panel[8*240+8]==0x8000&&panel[8*240+32]==0x8000);
    /* Successful modal opening detaches old cache instances only after LCD ack. */
    CHECK(ds_view_begin(app,DS_REPLACE,&tx)==DS_OK);
    CHECK(ds_view_modal_open(app,tx,DS_MODAL_SOLID,0x001122ff,7)==DS_OK);
    CHECK(ds_view_add(app,tx,&d,&ref)==DS_OK);
    CHECK(ds_view_submit(app,tx)==DS_OK);
    CHECK(ds_view_host_present(&host,&port,&stats)==DS_OK);
    CHECK(ds_view_host_route(&host,false)==DS_INPUT_MODAL&&host.modal.focus==7);
    CHECK(ds_view_cache_release(app,t)==DS_OK);
    CHECK(ds_view_cancel(app,tx)==DS_STALE); /* Late cancel cannot undo success. */
    CHECK(ds_view_begin(app,DS_REPLACE,&tx)==DS_OK);
    CHECK(ds_view_modal_close(app,tx)==DS_OK);
    CHECK(ds_view_submit(app,tx)==DS_INVALID); /* Missing background: auto abort. */
    CHECK(ds_view_host_route(&host,false)==DS_INPUT_MODAL);
    CHECK(ds_view_begin(app,DS_REPLACE,&tx)==DS_OK);
    CHECK(ds_view_modal_close(app,tx)==DS_OK);
    CHECK(ds_view_background(app,tx,0x000000ff)==DS_OK);
    CHECK(ds_view_submit(app,tx)==DS_OK);
    CHECK(ds_view_host_present(&host,&port,&stats)==DS_OK);
    CHECK(ds_view_host_route(&host,false)==DS_INPUT_APP&&host.modal.focus==42);
    /* Unsupported commands fail before a submission and release the builder. */
    CHECK(ds_view_begin(app,DS_REPLACE,&tx)==DS_OK);
    d.kind=DS_TEXT;CHECK(ds_view_add(app,tx,&d,&ref)==DS_UNSUPPORTED);
    CHECK(ds_view_submit(app,tx)==DS_STALE);
    CHECK(ds_view_begin(sys,DS_PATCH,&tx)==DS_OK);
    CHECK(ds_view_instantiate(sys,tx,t,&p,&b)==DS_STALE);
    CHECK(ds_view_begin(app,DS_PATCH,&tx)==DS_OK);
    ds_view_host_end_turn(&host);
    ds_view_host_init(&host,&core,&cache,0);
    CHECK(ds_view_cancel(app,app_ticket)==DS_STALE);
    CHECK(ds_view_cache_release(app,t)==DS_STALE);
    printf("view: PASS (coordinator=%zu B, cache/modal/IO/owner boundaries)\n",sizeof(host));
    return 0;
}
