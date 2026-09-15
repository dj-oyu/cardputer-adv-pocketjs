#include "core_fixture.h"
#include "ksn_view_host.h"
#include <stdio.h>
#include <string.h>

#define CHECK(x) do{if(!(x)){fprintf(stderr,"repair line %d: %s\n",__LINE__,#x);return 1;}}while(0)
KSN_TEST_CORE(core,static);
static ksn_cache cache;
static ksn_cache_command_block cache_commands;
static ksn_cache_text_block cache_text;
static ksn_view_host host;
static uint16_t strip[240*8],panel[240*135],committed[240*135];
static int fail_y=-1,invalidate_y=-1;
static unsigned transfers;
static bool no_strip;
static uint16_t *buffer(void *ctx){(void)ctx;return no_strip?NULL:strip;}
static ksn_result transfer(void *ctx,uint16_t y,uint16_t rows,const uint16_t *pixels){
    (void)ctx;transfers++;
    /* Failed transfers may already have changed the panel, including band 0. */
    memcpy(panel+y*240,pixels,rows*240*sizeof(*pixels));
    if(y==invalidate_y){invalidate_y=-1;ksn_view_host_invalidate(&host);}
    return y==fail_y?KSN_IO:KSN_OK;
}
static bool same_outcome(ksn_submission a,ksn_submission b){
    return a.ticket.value==b.ticket.value&&a.status==b.status&&a.reason==b.reason&&a.layer==b.layer;
}
static bool same_cache(ksn_cache_stats a,ksn_cache_stats b){
    return a.commands==b.commands&&a.text_bytes==b.text_bytes&&
           a.templates==b.templates&&a.instances==b.instances;
}
int main(void){
    CHECK(ksn_cache_bind(&cache,&cache_commands,&cache_text)==KSN_OK);
    ksn_view_host_init(&host,&core,&cache,42);
    ksn_view *app=ksn_view_host_endpoint(&host,KSN_APP),*system=ksn_view_host_endpoint(&host,KSN_SYSTEM);
    ksn_display_port port={NULL,buffer,transfer,240,135,8};ksn_render_stats stats;
    ksn_draw d={.kind=KSN_RECT,.bounds={12,12,50,50},.clip={0,0,240,135},
                .opacity=230,.data.shape={0xff8800ff,0,0}};
    ksn_placement placement={40,60,{0,0,240,135},180,true};
    ksn_template tpl;ksn_instance instance,discarded_instance;ksn_ref ref,system_ref,discarded_ref;
    ksn_tx tx,other;
    CHECK(ksn_view_cache_create(app,&d,1,&tpl)==KSN_OK);
    CHECK(ksn_view_begin(app,KSN_REPLACE,&tx)==KSN_OK);
    CHECK(ksn_view_background(app,tx,0x102030ff)==KSN_OK);
    CHECK(ksn_view_add(app,tx,&d,&ref)==KSN_OK);
    CHECK(ksn_view_instantiate(app,tx,tpl,&placement,&instance)==KSN_OK);
    CHECK(ksn_view_modal_open(app,tx,KSN_MODAL_DIM_LIVE,0x00000080,7)==KSN_OK);
    CHECK(ksn_view_submit(app,tx)==KSN_OK);
    CHECK(ksn_view_host_present(&host,&port,&stats)==KSN_OK);
    CHECK(ksn_view_begin(system,KSN_REPLACE,&tx)==KSN_OK);
    d.bounds=(ksn_rect){210,0,240,135};d.data.shape.color=0xabcdef80;
    CHECK(ksn_view_add(system,tx,&d,&system_ref)==KSN_OK);
    CHECK(ksn_view_submit(system,tx)==KSN_OK);
    CHECK(ksn_view_host_present(&host,&port,&stats)==KSN_OK);
    memcpy(committed,panel,sizeof(panel));
    ksn_submission system_outcome=ksn_view_poll(system);
    ksn_cache_stats cache_stats=ksn_cache_get_stats(&cache);

    for(unsigned band=0;band<17;band++){
        CHECK(ksn_view_begin(app,KSN_REPLACE,&tx)==KSN_OK);
        CHECK(ksn_view_modal_close(app,tx)==KSN_OK);
        CHECK(ksn_view_background(app,tx,0x40e080ff)==KSN_OK);
        CHECK(ksn_view_add(app,tx,&d,&discarded_ref)==KSN_OK);
        CHECK(ksn_view_instantiate(app,tx,tpl,&placement,&discarded_instance)==KSN_OK);
        CHECK(ksn_view_submit(app,tx)==KSN_OK);
        fail_y=(int)band*8;transfers=0;
        CHECK(ksn_view_host_present(&host,&port,&stats)==KSN_IO&&transfers==band+1);
        CHECK(memcmp(panel,committed,sizeof(panel))!=0);
        CHECK(ksn_view_cancel(app,tx)==KSN_OK);
        CHECK(!ksn_core_has_submission(&core)&&ksn_view_host_needs_present(&host));
        ksn_submission cancelled=ksn_view_poll(app),core_outcome=ksn_core_poll(&core);
        CHECK(cancelled.status==KSN_DISCARDED&&cancelled.reason==KSN_CANCELLED);
        CHECK(same_cache(ksn_cache_get_stats(&cache),cache_stats));
        CHECK(host.modal.phase==KSN_MODAL_OPEN&&host.modal.focus==7&&host.modal.saved_focus==42);
        CHECK(ksn_view_host_route(&host,false)==KSN_INPUT_BLOCKED);
        CHECK(ksn_view_host_route(&host,true)==KSN_INPUT_HOST);
        /* No begin/submit: retry the committed bank, failing that repair too. */
        transfers=0;
        CHECK(ksn_view_host_present(&host,&port,&stats)==KSN_IO&&transfers==band+1);
        CHECK(!ksn_core_has_submission(&core));
        CHECK(ksn_view_begin(app,KSN_PATCH,&other)==KSN_BUSY);
        CHECK(ksn_view_begin(system,KSN_PATCH,&other)==KSN_BUSY);
        CHECK(ksn_view_cancel(app,tx)==KSN_STALE);
        CHECK(same_outcome(ksn_core_poll(&core),core_outcome));
        CHECK(same_outcome(ksn_view_poll(app),cancelled));
        CHECK(same_outcome(ksn_view_poll(system),system_outcome));
        fail_y=-1;transfers=0;
        CHECK(ksn_view_host_present(&host,&port,&stats)==KSN_OK&&transfers==17);
        CHECK(stats.bands==0x1ffff&&stats.transferred_bytes==64800);
        CHECK(memcmp(panel,committed,sizeof(panel))==0);
        CHECK(!ksn_view_host_needs_present(&host));
        CHECK(ksn_view_host_route(&host,false)==KSN_INPUT_MODAL);
        CHECK(same_outcome(ksn_core_poll(&core),core_outcome));
        CHECK(same_outcome(ksn_view_poll(app),cancelled));
        CHECK(same_outcome(ksn_view_poll(system),system_outcome));
        CHECK(same_cache(ksn_cache_get_stats(&cache),cache_stats));
        CHECK(ksn_core_refs_active(&core,KSN_APP,ref,1));
        CHECK(ksn_core_refs_active(&core,KSN_SYSTEM,system_ref,1));
        CHECK(!ksn_core_refs_active(&core,KSN_APP,discarded_ref,1));
        CHECK(host.modal.phase==KSN_MODAL_OPEN&&host.modal.focus==7&&host.modal.saved_focus==42);
        transfers=0;CHECK(ksn_view_host_present(&host,&port,&stats)==KSN_OK&&transfers==0&&stats.bands==0);

        /* A host screen/indicator overwrite is repaired with no guest update.
         * Invalidation from any transfer, including the final one, survives. */
        memset(panel,0x5a,sizeof(panel));ksn_view_host_invalidate(&host);
        invalidate_y=(int)band*8;transfers=0;
        CHECK(ksn_view_host_present(&host,&port,&stats)==KSN_OK&&transfers==17);
        CHECK(memcmp(panel,committed,sizeof(panel))==0);
        CHECK(ksn_view_host_needs_present(&host));
        CHECK(same_outcome(ksn_view_poll(app),cancelled));
        transfers=0;
        CHECK(ksn_view_host_present(&host,&port,&stats)==KSN_OK&&transfers==17);
        CHECK(!ksn_view_host_needs_present(&host));
        CHECK(same_outcome(ksn_core_poll(&core),core_outcome));
        CHECK(same_outcome(ksn_view_poll(system),system_outcome));
    }
    /* A redraw arriving while a guest frame commits also survives its ack. */
    CHECK(ksn_view_begin(app,KSN_PATCH,&tx)==KSN_OK);
    CHECK(ksn_view_place(app,tx,instance,&placement)==KSN_OK);
    CHECK(ksn_view_submit(app,tx)==KSN_OK);
    ksn_view_host_invalidate(&host);invalidate_y=128;
    CHECK(ksn_view_host_present(&host,&port,&stats)==KSN_OK);
    CHECK(ksn_view_poll(app).status==KSN_PRESENTED&&ksn_view_poll(app).ticket.value==tx.value);
    CHECK(ksn_view_host_needs_present(&host));
    CHECK(ksn_view_host_present(&host,&port,&stats)==KSN_OK&&stats.transferred_bytes==64800);
    CHECK(ksn_view_poll(app).ticket.value==tx.value&&same_outcome(ksn_view_poll(system),system_outcome));
    CHECK(memcmp(panel,committed,sizeof(panel))==0);

    /* An active builder is never rendered by invalidation. */
    CHECK(ksn_view_begin(app,KSN_PATCH,&tx)==KSN_OK);
    ksn_change change={.property=KSN_SET_COLOR,.value.color=0x00ff00ff};
    CHECK(ksn_view_change(app,tx,ref,&change)==KSN_OK);
    ksn_view_host_invalidate(&host);transfers=0;
    CHECK(ksn_view_host_present(&host,&port,&stats)==KSN_BUSY&&transfers==0);
    CHECK(ksn_view_cancel(app,tx)==KSN_OK);
    CHECK(ksn_view_host_present(&host,&port,&stats)==KSN_OK);
    CHECK(memcmp(panel,committed,sizeof(panel))==0);
    ksn_view_host_invalidate(&host);no_strip=true;transfers=0;
    CHECK(ksn_view_host_present(&host,&port,&stats)==KSN_OOM&&transfers==0);
    CHECK(ksn_view_host_needs_present(&host));
    no_strip=false;
    CHECK(ksn_view_host_present(&host,&port,&stats)==KSN_OK&&stats.transferred_bytes==64800);
    ksn_view_host_invalidate(&host);fail_y=0;
    CHECK(ksn_view_host_present(&host,&port,&stats)==KSN_IO);
    ksn_frame frame;CHECK(ksn_core_frame(&core,&frame)==KSN_OK);
    ksn_view_host_init(&host,&core,&cache,0);
    CHECK(ksn_core_presented(&core,frame.ticket)==KSN_STALE);
    /* A direct low-level acknowledgement attests its submission; without
     * prepare_frame it cannot consume a separately requested host redraw. */
    ksn_client native=ksn_core_client(&core,KSN_APP);
    CHECK(native.ops->begin(native.ctx,KSN_REPLACE,&tx)==KSN_OK);
    CHECK(native.ops->background(native.ctx,tx,0x000000ff)==KSN_OK);
    d=(ksn_draw){.kind=KSN_TEXT,.bounds={0,0,10,10},.clip={0,0,240,135},.opacity=255,
                 .data.text={.utf8="x",.bytes=1,.capacity=1,.font=KSN_BODY,.color=0xffffffff}};
    CHECK(native.ops->add(native.ctx,tx,&d,&ref)==KSN_OK);
    CHECK(native.ops->end(native.ctx,tx)==KSN_OK);
    ksn_core_invalidate(&core);
    CHECK(ksn_core_presented(&core,tx)==KSN_OK&&ksn_core_needs_repair(&core));
    /* Unsupported committed commands can originate in another native renderer.
     * A rejected preflight must allow a replacement using this renderer. */
    fail_y=-1;transfers=0;
    CHECK(ksn_render_rects(&core,&port,&stats)==KSN_UNSUPPORTED&&transfers==0);
    CHECK(native.ops->begin(native.ctx,KSN_REPLACE,&tx)==KSN_OK);
    CHECK(native.ops->background(native.ctx,tx,0x000000ff)==KSN_OK);
    CHECK(native.ops->end(native.ctx,tx)==KSN_OK);
    CHECK(ksn_render_rects(&core,&port,&stats)==KSN_OK&&stats.transferred_bytes==64800);
    printf("committed repair: PASS (all 17 failed bands, cancel/retry, poll/cache/ref/modal preservation, invalidation during transfer)\n");
    return 0;
}
