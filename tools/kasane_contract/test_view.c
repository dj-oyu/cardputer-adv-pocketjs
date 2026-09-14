#include "ksn_view_host.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"view line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static ksn_core core;
static ksn_cache cache;
static uint16_t strip[240*8],panel[240*135];
static int fail_y=-1;
static uint16_t *buffer(void *ctx){(void)ctx;return strip;}
static ksn_result transfer(void *ctx,uint16_t y,uint16_t rows,const uint16_t *p){
    (void)ctx;memcpy(panel+y*240,p,rows*240*sizeof(*p));return y==fail_y?KSN_IO:KSN_OK;
}
int main(void){
    ksn_view_host host;ksn_view_host_init(&host,&core,&cache,42);
    ksn_view *app=ksn_view_host_endpoint(&host,KSN_APP),*sys=ksn_view_host_endpoint(&host,KSN_SYSTEM);
    ksn_display_port port={NULL,buffer,transfer,240,135,8};ksn_render_stats stats;
    ksn_draw d={.kind=KSN_RECT,.bounds={0,0,16,16},.clip={0,0,240,135},.opacity=255,.data.shape={0xff0000ff,0,0}};
    ksn_placement p={8,8,{0,0,240,135},128,true};
    ksn_template t;ksn_instance a,b;ksn_tx tx,other;ksn_ref ref;
    CHECK(ksn_view_features(app).modal&&!ksn_view_features(sys).modal);
    CHECK(ksn_view_features(app).draw_kinds==((1u<<KSN_RECT)|(1u<<KSN_ROUND_RECT)|
                                             (1u<<KSN_STROKE)|(1u<<KSN_GRADIENT)));
    CHECK(ksn_view_features(app).cache_kinds==((1u<<KSN_RECT)|(1u<<KSN_ROUND_RECT)|
                                               (1u<<KSN_STROKE)));
    CHECK(!ksn_view_features(app).animation&&!ksn_view_features(app).frosted);
    CHECK(ksn_view_cache_create(app,&d,1,&t)==KSN_OK);
    CHECK(ksn_view_cache_release(sys,t)==KSN_STALE);
    CHECK(ksn_view_begin(app,KSN_REPLACE,&tx)==KSN_OK);
    CHECK(ksn_view_background(app,tx,0x000000ff)==KSN_OK);
    CHECK(ksn_view_instantiate(app,tx,t,&p,&a)==KSN_OK);
    p.x=32;CHECK(ksn_view_instantiate(app,tx,t,&p,&b)==KSN_OK);
    CHECK(ksn_view_cancel(sys,tx)==KSN_STALE);
    CHECK(ksn_view_visible(sys,tx,a,false)==KSN_STALE);
    CHECK(ksn_view_begin(sys,KSN_PATCH,&other)==KSN_BUSY);
    CHECK(ksn_view_submit(app,tx)==KSN_OK);
    ksn_view_host_end_turn(&host); /* Submitted work must survive return/yield. */
    CHECK(ksn_view_poll(app).status==KSN_SUBMITTED);
    CHECK(ksn_view_host_present(&host,&port,&stats)==KSN_OK);
    CHECK(ksn_view_poll(app).status==KSN_PRESENTED);
    CHECK(panel[8*240+8]==0x8000&&panel[8*240+32]==0x8000);
    CHECK(ksn_view_cache_release(app,t)==KSN_BUSY);
    ksn_tx app_ticket=tx;
    CHECK(ksn_view_begin(sys,KSN_PATCH,&tx)==KSN_OK);
    CHECK(ksn_view_place(sys,tx,a,&p)==KSN_STALE);
    CHECK(ksn_view_begin(sys,KSN_REPLACE,&tx)==KSN_OK);
    CHECK(ksn_view_instantiate(sys,tx,t,&p,&b)==KSN_STALE);
    CHECK(ksn_view_begin(sys,KSN_REPLACE,&tx)==KSN_OK);
    CHECK(ksn_view_submit(sys,tx)==KSN_OK);
    CHECK(ksn_view_host_present(&host,&port,&stats)==KSN_OK);
    CHECK(ksn_view_poll(app).ticket.value==app_ticket.value);
    CHECK(ksn_view_poll(sys).ticket.value==tx.value);
    /* PATCH rollback restores placement/visibility, then next update works. */
    CHECK(ksn_view_begin(app,KSN_PATCH,&tx)==KSN_OK);
    CHECK(ksn_view_visible(app,tx,a,false)==KSN_OK);
    CHECK(ksn_view_cancel(app,tx)==KSN_OK);
    CHECK(ksn_view_begin(app,KSN_PATCH,&tx)==KSN_OK);
    CHECK(ksn_view_submit(app,tx)==KSN_OK);
    CHECK(ksn_view_host_present(&host,&port,&stats)==KSN_OK&&stats.bands==0);
    /* An unfinished modal and new cache instance are both cleaned on yield. */
    CHECK(ksn_view_begin(app,KSN_REPLACE,&tx)==KSN_OK);
    CHECK(ksn_view_modal_open(app,tx,KSN_MODAL_SOLID,0x001122ff,7)==KSN_OK);
    CHECK(ksn_view_instantiate(app,tx,t,&p,&b)==KSN_OK);
    ksn_view_host_end_turn(&host);
    CHECK(ksn_view_host_route(&host,false)==KSN_INPUT_APP);
    CHECK(ksn_cache_get_stats(&cache).instances==2);
    CHECK(ksn_view_begin(app,KSN_REPLACE,&tx)==KSN_OK);
    CHECK(ksn_view_modal_open(app,tx,KSN_MODAL_SOLID,0x001122ff,7)==KSN_OK);
    CHECK(ksn_view_submit(app,tx)==KSN_OK);
    fail_y=8;
    CHECK(ksn_view_host_present(&host,&port,&stats)==KSN_IO);
    CHECK(ksn_view_poll(app).status==KSN_SUBMITTED);
    CHECK(ksn_view_host_route(&host,false)==KSN_INPUT_BLOCKED);
    CHECK(ksn_view_host_route(&host,true)==KSN_INPUT_HOST);
    CHECK(ksn_view_cancel(app,tx)==KSN_OK);
    CHECK(ksn_view_poll(app).status==KSN_DISCARDED&&ksn_view_poll(app).reason==KSN_CANCELLED);
    CHECK(ksn_view_host_route(&host,false)==KSN_INPUT_BLOCKED);
    CHECK(ksn_view_begin(app,KSN_PATCH,&tx)==KSN_OK);
    CHECK(ksn_view_submit(app,tx)==KSN_OK);fail_y=-1;
    CHECK(ksn_view_host_present(&host,&port,&stats)==KSN_OK&&stats.transferred_bytes==64800);
    CHECK(ksn_view_host_route(&host,false)==KSN_INPUT_APP);
    CHECK(panel[8*240+8]==0x8000&&panel[8*240+32]==0x8000);
    /* Successful modal opening detaches old cache instances only after LCD ack. */
    CHECK(ksn_view_begin(app,KSN_REPLACE,&tx)==KSN_OK);
    CHECK(ksn_view_modal_open(app,tx,KSN_MODAL_SOLID,0x001122ff,7)==KSN_OK);
    CHECK(ksn_view_add(app,tx,&d,&ref)==KSN_OK);
    CHECK(ksn_view_submit(app,tx)==KSN_OK);
    CHECK(ksn_view_host_present(&host,&port,&stats)==KSN_OK);
    CHECK(ksn_view_host_route(&host,false)==KSN_INPUT_MODAL&&host.modal.focus==7);
    CHECK(ksn_view_cache_release(app,t)==KSN_OK);
    CHECK(ksn_view_cancel(app,tx)==KSN_STALE); /* Late cancel cannot undo success. */
    CHECK(ksn_view_begin(app,KSN_REPLACE,&tx)==KSN_OK);
    CHECK(ksn_view_modal_close(app,tx)==KSN_OK);
    CHECK(ksn_view_submit(app,tx)==KSN_INVALID); /* Missing background: auto abort. */
    CHECK(ksn_view_host_route(&host,false)==KSN_INPUT_MODAL);
    CHECK(ksn_view_begin(app,KSN_REPLACE,&tx)==KSN_OK);
    CHECK(ksn_view_modal_close(app,tx)==KSN_OK);
    CHECK(ksn_view_background(app,tx,0x000000ff)==KSN_OK);
    CHECK(ksn_view_submit(app,tx)==KSN_OK);
    CHECK(ksn_view_host_present(&host,&port,&stats)==KSN_OK);
    CHECK(ksn_view_host_route(&host,false)==KSN_INPUT_APP&&host.modal.focus==42);
    /* Unsupported commands fail before a submission and release the builder. */
    CHECK(ksn_view_begin(app,KSN_REPLACE,&tx)==KSN_OK);
    d.kind=KSN_TEXT;CHECK(ksn_view_add(app,tx,&d,&ref)==KSN_UNSUPPORTED);
    CHECK(ksn_view_submit(app,tx)==KSN_STALE);
    CHECK(ksn_view_begin(sys,KSN_PATCH,&tx)==KSN_OK);
    CHECK(ksn_view_instantiate(sys,tx,t,&p,&b)==KSN_STALE);
    CHECK(ksn_view_begin(app,KSN_PATCH,&tx)==KSN_OK);
    ksn_view_host_end_turn(&host);
    ksn_view_host_init(&host,&core,&cache,0);
    CHECK(ksn_view_cancel(app,app_ticket)==KSN_STALE);
    CHECK(ksn_view_cache_release(app,t)==KSN_STALE);
    printf("view: PASS (coordinator=%zu B, cache/modal/IO/owner boundaries)\n",sizeof(host));
    return 0;
}
