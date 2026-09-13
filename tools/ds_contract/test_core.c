#include "ds_core.h"
#include "use_cases.h"
#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { if(!(condition)){ \
    fprintf(stderr,"core failure at line %d: %s\n",__LINE__,#condition);return 1; \
} } while(0)

static ds_result begin_app(ds_client app,ds_update_mode mode,ds_tx *tx){
    ds_result result=app.ops->begin(app.ctx,mode,tx);
    if(result==DS_OK&&mode==DS_REPLACE)result=app.ops->background(app.ctx,*tx,0x0b1727ff);
    return result;
}
static ds_result presented(ds_core *core){
    ds_frame frame;ds_result result=ds_core_frame(core,&frame);
    return result==DS_OK?ds_core_presented(core,frame.ticket):result;
}
static ds_result discard(ds_core *core){
    ds_frame frame;ds_result result=ds_core_frame(core,&frame);
    return result==DS_OK?ds_core_discard(core,frame.ticket):result;
}
static ds_result add_rect(ds_client client,ds_tx tx,ds_ref *ref){
    ds_draw draw={.kind=DS_RECT,.bounds={1,2,11,12},.clip={0,0,240,135},.opacity=255};
    draw.data.shape.color=0x112233ff;
    return client.ops->add(client.ctx,tx,&draw,ref);
}
static ds_result add_text(ds_client client,ds_tx tx,const char *text,uint16_t bytes,
                          uint16_t capacity,ds_ref *ref){
    ds_draw draw={.kind=DS_TEXT,.bounds={1,2,100,16},.clip={0,0,240,135},.opacity=255};
    draw.data.text=(typeof(draw.data.text)){text,bytes,capacity,DS_BODY,0xf5eedcff};
    return client.ops->add(client.ctx,tx,&draw,ref);
}

static ds_result test_image_span(void *ctx,uint16_t variant,uint16_t frame,uint16_t y,
                                 uint16_t x,uint16_t count,uint16_t *rgb,uint8_t *alpha){
    (void)ctx;(void)variant;(void)frame;(void)y;(void)x;
    for(unsigned i=0;i<count;i++){rgb[i]=0xffff;alpha[i]=255;}
    return DS_OK;
}
int main(void){
    ds_core core;ds_core_init(&core);
    ds_client app=ds_core_client(&core,DS_APP),system=ds_core_client(&core,DS_SYSTEM);
    CHECK(app.ops&&system.ops&&ds_core_client(&core,(ds_layer)9).ops==NULL);
    ds_limits limits=app.ops->limits(app.ctx);
    CHECK(limits.app.commands==80&&limits.app.text_bytes==896);
    CHECK(limits.system.commands==16&&limits.system.text_bytes==128);
    CHECK(limits.native_bytes==DS_CORE_STORAGE_BYTES+3*sizeof(uint32_t));
    CHECK(limits.app.tracks==0&&limits.system.tracks==0);
    printf("core native budget: %lu bytes (including shared IDs)\n",(unsigned long)limits.native_bytes);

    pet_view view;
    ds_image_port image={NULL,64,64,16,16,test_image_span};ds_resource resource;
    CHECK(ds_core_register_image(&core,DS_APP,&image,&resource)==DS_OK);
    CHECK(pet_view_build(app,resource,&view)==DS_OK);
    CHECK(ds_core_has_submission(&core));
    CHECK(ds_core_active_usage(&core,DS_APP).commands==0);
    CHECK(ds_core_submission_usage(&core,DS_APP).commands==3);
    ds_tx blocked;
    CHECK(app.ops->begin(app.ctx,DS_PATCH,&blocked)==DS_BUSY);
    CHECK(presented(&core)==DS_OK&&!ds_core_has_submission(&core));
    CHECK(ds_core_active_usage(&core,DS_APP).commands==3);

    CHECK(pet_view_update(app,&view,100)==DS_OK);
    CHECK(presented(&core)==DS_OK);
    CHECK(notice_view_show(system,"ALARM",5)==DS_OK);
    CHECK(ds_core_submission_usage(&core,DS_APP).commands==3);
    CHECK(ds_core_submission_usage(&core,DS_SYSTEM).commands==1);
    CHECK(presented(&core)==DS_OK);

    /* A SYSTEM replace leaves APP references and generation intact. */
    CHECK(pet_view_update(app,&view,50)==DS_OK);
    CHECK(presented(&core)==DS_OK);

    /* Discard keeps the displayed baseline and old references usable. */
    ds_tx tx;ds_ref discarded;
    CHECK(begin_app(app,DS_REPLACE,&tx)==DS_OK);
    CHECK(add_rect(app,tx,&discarded)==DS_OK);
    CHECK(app.ops->end(app.ctx,tx)==DS_OK);
    CHECK(discard(&core)==DS_OK);
    CHECK(ds_core_active_usage(&core,DS_APP).commands==3);
    CHECK(pet_view_update(app,&view,40)==DS_OK);
    CHECK(presented(&core)==DS_OK);

    /* APP replace invalidates prior APP refs after presentation. */
    CHECK(begin_app(app,DS_REPLACE,&tx)==DS_OK);
    ds_ref replacement;CHECK(add_rect(app,tx,&replacement)==DS_OK);
    CHECK(app.ops->end(app.ctx,tx)==DS_OK&&presented(&core)==DS_OK);
    CHECK(app.ops->begin(app.ctx,DS_PATCH,&tx)==DS_OK);
    ds_change change={.property=DS_SET_VISIBLE,.value.visible=false};
    CHECK(app.ops->change(app.ctx,tx,view.pet,&change)==DS_STALE);
    CHECK(app.ops->end(app.ctx,tx)==DS_STALE);
    app.ops->abort(app.ctx,tx);

    /* Invalid text poisons only the open transaction and publishes nothing. */
    CHECK(begin_app(app,DS_REPLACE,&tx)==DS_OK);
    ds_ref text;
    CHECK(add_text(app,tx,"A\n",2,8,&text)==DS_INVALID);
    CHECK(add_rect(app,tx,&text)==DS_INVALID);
    CHECK(app.ops->end(app.ctx,tx)==DS_INVALID);
    app.ops->abort(app.ctx,tx);
    CHECK(ds_core_active_usage(&core,DS_APP).commands==1);

    ds_core_init(&core);app=ds_core_client(&core,DS_APP);
    CHECK(begin_app(app,DS_REPLACE,&tx)==DS_OK);
    for(unsigned i=0;i<7;i++)CHECK(add_text(app,tx,"",0,128,&text)==DS_OK);
    CHECK(add_text(app,tx,"",0,1,&text)==DS_LIMIT);
    CHECK(app.ops->end(app.ctx,tx)==DS_LIMIT);
    app.ops->abort(app.ctx,tx);
    CHECK(!ds_core_has_submission(&core));

    ds_core_init(&core);app=ds_core_client(&core,DS_APP);
    CHECK(begin_app(app,DS_REPLACE,&tx)==DS_OK);
    CHECK(add_text(app,tx,"12345",5,4,&text)==DS_LIMIT);
    app.ops->abort(app.ctx,tx);

    ds_core_init(&core);app=ds_core_client(&core,DS_APP);
    CHECK(begin_app(app,DS_REPLACE,&tx)==DS_OK);
    for(unsigned i=0;i<DS_APP_COMMANDS;i++)CHECK(add_rect(app,tx,&replacement)==DS_OK);
    CHECK(add_rect(app,tx,&replacement)==DS_LIMIT);
    app.ops->abort(app.ctx,tx);

    puts("fixed core: PASS");return 0;
}
