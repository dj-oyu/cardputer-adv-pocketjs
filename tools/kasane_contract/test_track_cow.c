#include "core_fixture.h"
#include "ksn_core.h"
#include "ksn_p0_probe.h"
#include <assert.h>
#include <stdio.h>

static unsigned track_copies;
static size_t track_bytes;
void ksn_p0_probe_copy(ksn_p0_copy_kind kind,size_t bytes){
    if(kind==KSN_P0_CORE_CLONE_TRACK&&bytes){track_copies++;track_bytes+=bytes;}
}
bool ksn_p0_probe_watch_source_text(const char *text,size_t bytes){
    (void)text;(void)bytes;return true;
}
void ksn_p0_probe_unwatch_source_text(const char *text){(void)text;}
void ksn_p0_probe_core_source_text(const char *text,size_t bytes){(void)text;(void)bytes;}

static ksn_result source(void *context,uint16_t variant,uint16_t frame,uint16_t y,
                         uint16_t x,uint16_t count,uint16_t *rgb,uint8_t *alpha){
    (void)context;(void)variant;(void)frame;(void)y;(void)x;
    for(unsigned i=0;i<count;i++){rgb[i]=0xffff;alpha[i]=255;}
    return KSN_OK;
}

int main(void){
    KSN_TEST_CORE(core,static);
    ksn_core_init(&core);
    ksn_core_animation_block animations[2];
    assert(ksn_core_enable_animation(&core,&animations[0],&animations[1])==KSN_OK);
    ksn_client app=ksn_core_client(&core,KSN_APP);
    ksn_image_port image={NULL,64,64,1,1,source};
    ksn_resource resource;
    assert(ksn_core_register_image(&core,KSN_APP,&image,&resource)==KSN_OK);
    ksn_draw draw={.kind=KSN_IMAGE,.bounds={10,20,42,52},.clip={0,0,240,135},.opacity=255,
        .data.image={.resource=resource,.scale=KSN_IMAGE_STRETCH,
                     .source_width=64,.source_height=64}};
    ksn_tx tx;ksn_ref ref;ksn_animation animation;
    assert(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    assert(app.ops->background(app.ctx,tx,0x000000ff)==KSN_OK);
    assert(app.ops->add(app.ctx,tx,&draw,&ref)==KSN_OK);
    ksn_motion motion={.first=ref,.count=1,.property=KSN_TRANSFORM,
        .from.pose={{10,20,42,52},0},.to.pose={{20,20,52,52},0},
        .duration_ms=1000,.easing=KSN_LINEAR,.repeat=KSN_ONCE};
    assert(app.ops->animate(app.ctx,tx,&motion,&animation)==KSN_OK);
    assert(app.ops->end(app.ctx,tx)==KSN_OK);
    assert(ksn_core_presented(&core,tx)==KSN_OK);
    assert(track_copies==0&&track_bytes==0);

    assert(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
    assert(app.ops->end(app.ctx,tx)==KSN_OK);
    assert(ksn_core_presented(&core,tx)==KSN_OK);
    assert(track_copies==0&&track_bytes==0);
    assert(ksn_core_poll_animation(&core,KSN_APP,animation)==KSN_ANIMATION_PENDING);

    assert(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
    assert(app.ops->stop(app.ctx,tx,animation)==KSN_OK);
    assert(track_copies==1&&track_bytes==sizeof(ksn_track));
    assert(app.ops->end(app.ctx,tx)==KSN_OK);
    assert(ksn_core_presented(&core,tx)==KSN_OK);
    assert(ksn_core_poll_animation(&core,KSN_APP,animation)==KSN_ANIMATION_STOPPED);

    assert(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
    assert(app.ops->stop(app.ctx,tx,animation)==KSN_OK);
    assert(app.ops->end(app.ctx,tx)==KSN_OK);
    assert(ksn_core_presented(&core,tx)==KSN_OK);
    assert(track_copies==1&&track_bytes==sizeof(ksn_track));

    /* Reusing a stopped slot assigns every field and must not clone it. */
    ksn_animation replacement;
    assert(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
    assert(app.ops->animate(app.ctx,tx,&motion,&replacement)==KSN_OK);
    assert(replacement.value!=animation.value);
    assert(track_copies==1&&track_bytes==sizeof(ksn_track));
    assert(app.ops->end(app.ctx,tx)==KSN_OK);
    assert(ksn_core_presented(&core,tx)==KSN_OK);
    assert(ksn_core_poll_animation(&core,KSN_APP,replacement)==KSN_ANIMATION_PENDING);
    printf("track COW PASS: no-op=0, one changed slot=%zu B, repeated stop=0, reuse=0\n",track_bytes);
    return 0;
}
