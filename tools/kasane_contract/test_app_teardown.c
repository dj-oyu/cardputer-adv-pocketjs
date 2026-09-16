#include "core_fixture.h"
#include "ksn_view_host.h"
#include <stdio.h>
#include <string.h>

#define CHECK(c) do { if(!(c)){fprintf(stderr,"APP teardown line %d: %s\n",__LINE__,#c);return 1;} } while(0)
static uint16_t panel[240*135],strip[240*8];
static int fail_band=-1;
static ksn_view_host *reentrant;
static bool reentry_ok;
static uint16_t *buffer(void *ctx){(void)ctx;return strip;}
static ksn_result send(void *ctx,uint16_t y0,uint16_t rows,const uint16_t *pixels){
    (void)ctx;
    if(reentrant){
        ksn_view_host *h=reentrant;reentrant=NULL;
        ksn_render_stats stats;
        reentry_ok=ksn_view_host_reset_app(h)==KSN_BUSY&&
                   ksn_view_host_present(h,NULL,&stats)==KSN_BUSY;
    }
    if(y0/8==fail_band)return KSN_IO;
    memcpy(panel+y0*240,pixels,rows*240*2);
    return KSN_OK;
}
static ksn_result image_span(void *ctx,uint16_t v,uint16_t f,uint16_t y,
                             uint16_t x,uint16_t n,uint16_t *rgb,uint8_t *alpha){
    (void)ctx;(void)v;(void)f;(void)y;(void)x;(void)n;(void)rgb;(void)alpha;
    return KSN_OK;
}
int main(void){
    for(unsigned scenario=0;scenario<7;scenario++){
        KSN_TEST_CORE(core,);
        ksn_cache cache;ksn_cache_command_block commands;ksn_cache_text_block text;
        CHECK(ksn_cache_bind(&cache,&commands,&text)==KSN_OK);
        ksn_view_host host;ksn_view_host_init(&host,&core,&cache,12);
        ksn_view *app=ksn_view_host_endpoint(&host,KSN_APP),*sys=ksn_view_host_endpoint(&host,KSN_SYSTEM);
        ksn_display_port port={NULL,buffer,send,240,135,8,NULL};ksn_render_stats stats;
        ksn_draw d={.kind=KSN_RECT,.bounds={0,0,8,8},.clip={0,0,240,135},.opacity=255,
                    .data.shape={0xff0000ff,0,0}};
        ksn_template a,s,a2;ksn_instance ai,si;ksn_tx tx;
        CHECK(ksn_view_cache_create(app,&d,1,&a)==KSN_OK);
        d.data.shape.color=0x00ff00ff;
        CHECK(ksn_view_cache_create(sys,&d,1,&s)==KSN_OK);
        CHECK(ksn_view_cache_create(app,&d,1,&a2)==KSN_OK);
        ksn_image_port image={NULL,1,1,1,1,image_span};ksn_resource ar,sr;
        CHECK(ksn_core_register_image(&core,KSN_APP,&image,&ar)==KSN_OK);
        CHECK(ksn_core_register_image(&core,KSN_SYSTEM,&image,&sr)==KSN_OK);
        ksn_placement p={0,0,{0,0,240,135},255,true};
        CHECK(ksn_view_begin(app,KSN_REPLACE,&tx)==KSN_OK);
        CHECK(ksn_view_modal_open(app,tx,KSN_MODAL_SOLID,0x101010ff,88)==KSN_OK);
        CHECK(ksn_view_instantiate(app,tx,a,&p,&ai)==KSN_OK);
        CHECK(ksn_view_submit(app,tx)==KSN_OK);
        CHECK(ksn_view_host_present(&host,&port,&stats)==KSN_OK);
        p.x=16;
        CHECK(ksn_view_begin(sys,KSN_REPLACE,&tx)==KSN_OK);
        CHECK(ksn_view_instantiate(sys,tx,s,&p,&si)==KSN_OK);
        CHECK(ksn_view_submit(sys,tx)==KSN_OK);
        reentrant=&host;reentry_ok=false;
        CHECK(ksn_view_host_present(&host,&port,&stats)==KSN_OK&&reentry_ok);
        CHECK(panel[0]==0xf800&&panel[16]==0x07e0);
        ksn_submission system_before=ksn_view_poll(sys);
        if(scenario>=1&&scenario<=3){
            CHECK(ksn_view_begin(app,KSN_REPLACE,&tx)==KSN_OK);
            CHECK(ksn_view_modal_close(app,tx)==KSN_OK);
            CHECK(ksn_view_background(app,tx,0xffffffff)==KSN_OK);
            p.x=32;CHECK(ksn_view_instantiate(app,tx,a2,&p,&ai)==KSN_OK);
            if(scenario>=2)CHECK(ksn_view_submit(app,tx)==KSN_OK);
            if(scenario==3){fail_band=1;CHECK(ksn_view_host_present(&host,&port,&stats)==KSN_IO);fail_band=-1;}
        }else if(scenario>=4&&scenario<=5){
            CHECK(ksn_view_begin(sys,KSN_PATCH,&tx)==KSN_OK);
            p.x=24;CHECK(ksn_view_place(sys,tx,si,&p)==KSN_OK);
            if(scenario==5)CHECK(ksn_view_submit(sys,tx)==KSN_OK);
            system_before=ksn_view_poll(sys);
        }else if(scenario==6){
            /* No pending guest submission: repair borrows the active bank.
             * IO failure must not keep the dead APP leased indefinitely. */
            ksn_view_host_invalidate(&host);fail_band=1;
            CHECK(ksn_view_host_present(&host,&port,&stats)==KSN_IO);
            CHECK(core.state.repairing);fail_band=-1;
        }
        CHECK(ksn_view_host_reset_app(&host)==KSN_OK);
        CHECK(ksn_core_active_usage(&core,KSN_APP).commands==0);
        CHECK(ksn_core_submission_usage(&core,KSN_APP).commands==0);
        CHECK(host.modal.phase==KSN_MODAL_CLOSED&&host.modal.focus==0);
        CHECK(ksn_view_host_route(&host,false)==KSN_INPUT_BLOCKED);
        CHECK(ksn_view_poll(sys).ticket.value==system_before.ticket.value&&
              ksn_view_poll(sys).status==system_before.status);
        CHECK(core.state.image_count==1&&core.state.images[0].id.value==sr.value);
        CHECK(ksn_cache_get_stats(&cache).templates==1&&ksn_cache_get_stats(&cache).instances==1);
        CHECK(ksn_cache_release(&cache,a)==KSN_STALE&&ksn_cache_release(&cache,a2)==KSN_STALE);
        CHECK(ksn_view_cancel(app,tx)==KSN_STALE);
        if(scenario==4)CHECK(ksn_view_submit(sys,tx)==KSN_OK);
        ksn_result result=ksn_view_host_present(&host,&port,&stats);
        if(result!=KSN_OK||stats.bands!=0x1ffff)fprintf(stderr,"scenario=%u result=%d bands=%u\n",scenario,result,stats.bands);
        CHECK(result==KSN_OK&&stats.bands==0x1ffff);
        unsigned sx=scenario>=4&&scenario<=5?24:16;
        for(unsigned y=0;y<135;y++)for(unsigned x=0;x<240;x++)
            CHECK(panel[y*240+x]==(y<8&&x>=sx&&x<sx+8?0x07e0:0));
        /* The survivor still patches after cache compaction and bank swaps. */
        CHECK(ksn_view_begin(sys,KSN_PATCH,&tx)==KSN_OK);
        p.x=40;CHECK(ksn_view_place(sys,tx,si,&p)==KSN_OK);
        CHECK(ksn_view_submit(sys,tx)==KSN_OK);
        CHECK(ksn_view_host_present(&host,&port,&stats)==KSN_OK&&panel[40]==0x07e0);
        /* Repeated exits reclaim the same quotas without resetting SYSTEM. */
        for(unsigned i=0;i<100;i++){
            CHECK(ksn_view_cache_create(app,&d,1,&a)==KSN_OK);
            CHECK(ksn_core_register_image(&core,KSN_APP,&image,&ar)==KSN_OK);
            CHECK(ksn_view_host_reset_app(&host)==KSN_OK);
            CHECK(ksn_cache_get_stats(&cache).templates==1&&core.state.image_count==1);
        }
    }
    puts("APP teardown: PASS (SYSTEM committed/building/submitted, modal/cache/images, partial IO, reentry, 100 resets)");
    return 0;
}
