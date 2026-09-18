#include "core_fixture.h"
#include "ksn_render.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint16_t strip[240*8];static bool fail;
static uint16_t *buffer(void *p){(void)p;return strip;}
static ksn_result send(void *p,uint16_t y,uint16_t n,const uint16_t *rgb){(void)p;(void)y;(void)n;(void)rgb;return fail?KSN_IO:KSN_OK;}
static ksn_result source(void *p,uint16_t v,uint16_t f,uint16_t y,uint16_t x,uint16_t n,uint16_t *rgb,uint8_t *alpha){
    (void)p;assert(!v&&!f&&y<64&&x+n<=64);
    for(unsigned i=0;i<n;i++){rgb[i]=0xffff;alpha[i]=255;}return KSN_OK;
}
static void pose(ksn_core *core,ksn_tx tx,int x,int size,unsigned rotation){
    ksn_frame_command c;assert(ksn_core_read(core,tx,false,KSN_APP,0,&c)==KSN_OK);
    assert(c.draw.bounds.x0==x&&c.draw.bounds.x1==x+size&&c.draw.data.image.rotation==rotation);
}
int main(void){
    KSN_TEST_CORE(core,);ksn_core_init(&core);ksn_core_animation_block blocks[2];
    assert(ksn_core_enable_animation(&core,&blocks[0],&blocks[1])==KSN_OK);
    assert(ksn_core_animation_bytes(&core)<=1024);
    ksn_client app=ksn_core_client(&core,KSN_APP);ksn_image_port image={NULL,64,64,1,1,source};ksn_resource resource;
    assert(ksn_core_register_image(&core,KSN_APP,&image,&resource)==KSN_OK);
    ksn_display_port display={NULL,buffer,send,240,135,8,NULL,NULL};ksn_render_stats stats;
    ksn_tx tx;ksn_ref ref;ksn_animation animation;
    ksn_draw d={.kind=KSN_IMAGE,.bounds={10,20,42,52},.clip={0,0,240,135},.opacity=255,
        .data.image={.resource=resource,.scale=KSN_IMAGE_STRETCH,.source_width=64,.source_height=64}};
    ksn_motion m={.count=1,.property=KSN_TRANSFORM,.from.pose={{10,20,42,52},0},
        .to.pose={{110,40,206,136},2048},.duration_ms=1000,.easing=KSN_LINEAR,.repeat=KSN_ONCE};
    assert(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    assert(app.ops->background(app.ctx,tx,255)==KSN_OK&&app.ops->add(app.ctx,tx,&d,&ref)==KSN_OK);m.first=ref;
    assert(app.ops->animate(app.ctx,tx,&m,&animation)==KSN_OK&&app.ops->end(app.ctx,tx)==KSN_OK);
    assert(ksn_core_poll_animation(&core,KSN_APP,animation)==KSN_ANIMATION_PENDING);
    ksn_core_start_animations(&core,1000000);
    assert(ksn_core_animation_deadline(&core)==UINT64_MAX); // Still not presented.
    assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);ksn_core_start_animations(&core,2000000);
    assert(ksn_core_animation_deadline(&core)==2033334);
    assert(ksn_core_advance_animations(&core,2010000,false,&tx)==KSN_OK&&!tx.value);
    assert(ksn_core_advance_animations(&core,2250000,false,&tx)==KSN_OK&&tx.value);pose(&core,tx,35,48,512);
    fail=true;assert(ksn_render_rects(&core,&display,&stats)==KSN_IO);
    ksn_tx ignored;assert(ksn_core_advance_animations(&core,2990000,false,&ignored)==KSN_BUSY);pose(&core,tx,35,48,512);
    fail=false;assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    assert(ksn_core_advance_animations(&core,2750000,false,&tx)==KSN_OK);pose(&core,tx,85,80,512);
    assert(ksn_core_discard(&core,tx)==KSN_OK);
    assert(ksn_core_advance_animations(&core,3100000,false,&tx)==KSN_OK);pose(&core,tx,110,96,0);
    assert(ksn_core_poll_animation(&core,KSN_APP,animation)==KSN_ANIMATION_RUNNING);
    assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    assert(ksn_core_poll_animation(&core,KSN_APP,animation)==KSN_ANIMATION_FINISHED);
    assert(ksn_core_animation_deadline(&core)==UINT64_MAX);
    // Loop/pingpong and stop/finish retain the last presented state until ack.
    for(unsigned easing=0;easing<=KSN_STEP;easing++)for(unsigned repeat=KSN_LOOP;repeat<=KSN_PINGPONG;repeat++){
        m.easing=(ksn_easing)easing;m.repeat=(ksn_repeat)repeat;
        assert(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK&&app.ops->animate(app.ctx,tx,&m,&animation)==KSN_OK);
        assert(app.ops->end(app.ctx,tx)==KSN_OK&&ksn_render_rects(&core,&display,&stats)==KSN_OK);
        ksn_core_start_animations(&core,5000000);
        assert(ksn_core_advance_animations(&core,6500000,false,&tx)==KSN_OK);
        pose(&core,tx,easing==KSN_STEP?10:easing==KSN_EASE_OUT_CUBIC?98:60,
             easing==KSN_STEP?32:easing==KSN_EASE_OUT_CUBIC?88:64,easing==KSN_STEP?0:easing==KSN_EASE_OUT_CUBIC?768:0);
        assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);
        assert(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
        ksn_change change={.property=KSN_SET_RECT,.value.rect=d.bounds};
        assert(app.ops->change(app.ctx,tx,ref,&change)==KSN_BUSY);app.ops->abort(app.ctx,tx);
        assert(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK&&app.ops->stop(app.ctx,tx,animation)==KSN_OK);
        app.ops->abort(app.ctx,tx);assert(ksn_core_poll_animation(&core,KSN_APP,animation)==KSN_ANIMATION_RUNNING);
        assert(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK&&ksn_core_finish_animation(&core,KSN_APP,tx,animation)==KSN_OK);
        assert(app.ops->end(app.ctx,tx)==KSN_OK);
        pose(&core,tx,110,96,0);
        assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    }
    assert(ksn_core_reset_layer(&core,KSN_APP)==KSN_OK);
    assert(ksn_core_poll_animation(&core,KSN_APP,animation)==KSN_ANIMATION_DISCARDED);
    ksn_animation ids[2][6];
    for(unsigned layer=0;layer<2;layer++){
        ksn_client client=ksn_core_client(&core,(ksn_layer)layer);
        assert(ksn_core_register_image(&core,(ksn_layer)layer,&image,&resource)==KSN_OK);
        d.data.image.resource=resource;unsigned limit=layer?2:6;
        assert(client.ops->begin(client.ctx,KSN_REPLACE,&tx)==KSN_OK);
        if(!layer)assert(client.ops->background(client.ctx,tx,255)==KSN_OK);
        for(unsigned i=0;i<=limit;i++){
            assert(client.ops->add(client.ctx,tx,&d,&ref)==KSN_OK);m.first=ref;
            assert(client.ops->animate(client.ctx,tx,&m,&animation)==(i==limit?KSN_LIMIT:KSN_OK));
        }
        client.ops->abort(client.ctx,tx);
        assert(client.ops->begin(client.ctx,KSN_REPLACE,&tx)==KSN_OK);
        if(!layer)assert(client.ops->background(client.ctx,tx,255)==KSN_OK);
        for(unsigned i=0;i<limit;i++){
            assert(client.ops->add(client.ctx,tx,&d,&ref)==KSN_OK);m.first=ref;
            assert(client.ops->animate(client.ctx,tx,&m,&ids[layer][i])==KSN_OK);
        }
        assert(client.ops->end(client.ctx,tx)==KSN_OK&&ksn_render_rects(&core,&display,&stats)==KSN_OK);
        ksn_core_start_animations(&core,9000000);
        assert(ksn_core_active_usage(&core,(ksn_layer)layer).tracks==limit);
    }
    assert(ksn_core_reset_layer(&core,KSN_APP)==KSN_OK);
    assert(ksn_core_poll_animation(&core,KSN_APP,ids[0][0])==KSN_ANIMATION_DISCARDED);
    assert(ksn_core_poll_animation(&core,KSN_SYSTEM,ids[1][0])==KSN_ANIMATION_RUNNING);
    assert(ksn_core_advance_animations(&core,9000001,true,&tx)==KSN_OK&&tx.value);
    assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    assert(ksn_core_poll_animation(&core,KSN_SYSTEM,ids[1][0])==KSN_ANIMATION_FINISHED);
    assert(ksn_core_animation_deadline(&core)==UINT64_MAX);
    puts("animation PASS: delayed start, rational interpolation, multi-turn, IO snapshot, drop/catch-up, easing/repeat, stop/finish/reset");
}
