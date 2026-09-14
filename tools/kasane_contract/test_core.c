#include "ksn_core.h"
#include "use_cases.h"
#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { if(!(condition)){ \
    fprintf(stderr,"core failure at line %d: %s\n",__LINE__,#condition);return 1; \
} } while(0)

static ksn_result begin_app(ksn_client app,ksn_update_mode mode,ksn_tx *tx){
    ksn_result result=app.ops->begin(app.ctx,mode,tx);
    if(result==KSN_OK&&mode==KSN_REPLACE)result=app.ops->background(app.ctx,*tx,0x0b1727ff);
    return result;
}
static ksn_result presented(ksn_core *core){
    ksn_frame frame;ksn_result result=ksn_core_frame(core,&frame);
    return result==KSN_OK?ksn_core_presented(core,frame.ticket):result;
}
static ksn_result discard(ksn_core *core){
    ksn_frame frame;ksn_result result=ksn_core_frame(core,&frame);
    return result==KSN_OK?ksn_core_discard(core,frame.ticket):result;
}
static ksn_result add_rect(ksn_client client,ksn_tx tx,ksn_ref *ref){
    ksn_draw draw={.kind=KSN_RECT,.bounds={1,2,11,12},.clip={0,0,240,135},.opacity=255};
    draw.data.shape.color=0x112233ff;
    return client.ops->add(client.ctx,tx,&draw,ref);
}
static ksn_result add_text(ksn_client client,ksn_tx tx,const char *text,uint16_t bytes,
                          uint16_t capacity,ksn_ref *ref){
    ksn_draw draw={.kind=KSN_TEXT,.bounds={1,2,100,16},.clip={0,0,240,135},.opacity=255};
    draw.data.text=(typeof(draw.data.text)){text,bytes,capacity,KSN_BODY,0xf5eedcff};
    return client.ops->add(client.ctx,tx,&draw,ref);
}

static ksn_result test_image_span(void *ctx,uint16_t variant,uint16_t frame,uint16_t y,
                                 uint16_t x,uint16_t count,uint16_t *rgb,uint8_t *alpha){
    (void)ctx;(void)variant;(void)frame;(void)y;(void)x;
    for(unsigned i=0;i<count;i++){rgb[i]=0xffff;alpha[i]=255;}
    return KSN_OK;
}
int main(void){
    ksn_core core;ksn_core_init(&core);
    ksn_client app=ksn_core_client(&core,KSN_APP),system=ksn_core_client(&core,KSN_SYSTEM);
    CHECK(app.ops&&system.ops&&ksn_core_client(&core,(ksn_layer)9).ops==NULL);
    ksn_limits limits=app.ops->limits(app.ctx);
    CHECK(limits.app.commands==80&&limits.app.text_bytes==896);
    CHECK(limits.system.commands==16&&limits.system.text_bytes==128);
    CHECK(limits.native_bytes==KSN_CORE_STORAGE_BYTES+3*sizeof(uint32_t));
    CHECK(limits.app.tracks==0&&limits.system.tracks==0);
    printf("core native budget: %lu bytes (including shared IDs)\n",(unsigned long)limits.native_bytes);

    pet_view view;
    ksn_image_port image={NULL,64,64,16,16,test_image_span};ksn_resource resource;
    CHECK(ksn_core_register_image(&core,KSN_APP,&image,&resource)==KSN_OK);
    CHECK(pet_view_build(app,resource,&view)==KSN_OK);
    CHECK(ksn_core_has_submission(&core));
    CHECK(ksn_core_active_usage(&core,KSN_APP).commands==0);
    CHECK(ksn_core_submission_usage(&core,KSN_APP).commands==3);
    ksn_tx blocked;
    CHECK(app.ops->begin(app.ctx,KSN_PATCH,&blocked)==KSN_BUSY);
    CHECK(presented(&core)==KSN_OK&&!ksn_core_has_submission(&core));
    CHECK(ksn_core_active_usage(&core,KSN_APP).commands==3);

    CHECK(pet_view_update(app,&view,100)==KSN_OK);
    CHECK(presented(&core)==KSN_OK);
    CHECK(notice_view_show(system,"ALARM",5)==KSN_OK);
    CHECK(ksn_core_submission_usage(&core,KSN_APP).commands==3);
    CHECK(ksn_core_submission_usage(&core,KSN_SYSTEM).commands==1);
    CHECK(presented(&core)==KSN_OK);

    /* A SYSTEM replace leaves APP references and generation intact. */
    CHECK(pet_view_update(app,&view,50)==KSN_OK);
    CHECK(presented(&core)==KSN_OK);

    /* Discard keeps the displayed baseline and old references usable. */
    ksn_tx tx;ksn_ref discarded;
    CHECK(begin_app(app,KSN_REPLACE,&tx)==KSN_OK);
    CHECK(add_rect(app,tx,&discarded)==KSN_OK);
    CHECK(app.ops->end(app.ctx,tx)==KSN_OK);
    CHECK(discard(&core)==KSN_OK);
    CHECK(ksn_core_active_usage(&core,KSN_APP).commands==3);
    CHECK(pet_view_update(app,&view,40)==KSN_OK);
    CHECK(presented(&core)==KSN_OK);

    /* APP replace invalidates prior APP refs after presentation. */
    CHECK(begin_app(app,KSN_REPLACE,&tx)==KSN_OK);
    ksn_ref replacement;CHECK(add_rect(app,tx,&replacement)==KSN_OK);
    CHECK(app.ops->end(app.ctx,tx)==KSN_OK&&presented(&core)==KSN_OK);
    CHECK(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
    ksn_change change={.property=KSN_SET_VISIBLE,.value.visible=false};
    CHECK(app.ops->change(app.ctx,tx,view.pet,&change)==KSN_STALE);
    CHECK(app.ops->end(app.ctx,tx)==KSN_STALE);
    app.ops->abort(app.ctx,tx);

    /* Invalid text poisons only the open transaction and publishes nothing. */
    CHECK(begin_app(app,KSN_REPLACE,&tx)==KSN_OK);
    ksn_ref text;
    CHECK(add_text(app,tx,"A\n",2,8,&text)==KSN_INVALID);
    CHECK(add_rect(app,tx,&text)==KSN_INVALID);
    CHECK(app.ops->end(app.ctx,tx)==KSN_INVALID);
    app.ops->abort(app.ctx,tx);
    CHECK(ksn_core_active_usage(&core,KSN_APP).commands==1);

    ksn_core_init(&core);app=ksn_core_client(&core,KSN_APP);
    CHECK(begin_app(app,KSN_REPLACE,&tx)==KSN_OK);
    for(unsigned i=0;i<7;i++)CHECK(add_text(app,tx,"",0,128,&text)==KSN_OK);
    CHECK(add_text(app,tx,"",0,1,&text)==KSN_LIMIT);
    CHECK(app.ops->end(app.ctx,tx)==KSN_LIMIT);
    app.ops->abort(app.ctx,tx);
    CHECK(!ksn_core_has_submission(&core));

    ksn_core_init(&core);app=ksn_core_client(&core,KSN_APP);
    CHECK(begin_app(app,KSN_REPLACE,&tx)==KSN_OK);
    CHECK(add_text(app,tx,"12345",5,4,&text)==KSN_LIMIT);
    app.ops->abort(app.ctx,tx);

    ksn_core_init(&core);app=ksn_core_client(&core,KSN_APP);
    CHECK(begin_app(app,KSN_REPLACE,&tx)==KSN_OK);
    for(unsigned i=0;i<KSN_APP_COMMANDS;i++)CHECK(add_rect(app,tx,&replacement)==KSN_OK);
    CHECK(add_rect(app,tx,&replacement)==KSN_LIMIT);
    app.ops->abort(app.ctx,tx);

    puts("fixed core: PASS");return 0;
}
