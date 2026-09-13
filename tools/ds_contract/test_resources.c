#include "ds_core.h"
#include <stdio.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"resource line %d: %s\n",__LINE__,#x);return 1;}} while(0)
static unsigned calls;
static ds_result span(void *ctx,uint16_t variant,uint16_t frame,uint16_t y,uint16_t x,
                       uint16_t count,uint16_t *rgb,uint8_t *alpha){
    (void)ctx;(void)y;(void)x;calls++;
    for(unsigned i=0;i<count;i++){rgb[i]=(uint16_t)(variant*16+frame);alpha[i]=255;}
    return DS_OK;
}
int main(void){
    ds_core core;ds_core_init(&core);ds_client app=ds_core_client(&core,DS_APP),sys=ds_core_client(&core,DS_SYSTEM);
    ds_image_port port={NULL,64,64,2,3,span};ds_resource image,other;
    CHECK(ds_core_register_image(&core,DS_APP,&port,&image)==DS_OK);
    port.width=1; /* The descriptor was copied. */
    CHECK(ds_core_register_image(&core,DS_SYSTEM,&port,&other)==DS_OK);
    ds_tx tx;ds_ref ref;
    ds_draw d={.kind=DS_IMAGE,.bounds={0,0,64,64},.clip={0,0,240,135},.opacity=255};
    d.data.image.resource=image;d.data.image.variant=1;d.data.image.frame=2;
    CHECK(sys.ops->begin(sys.ctx,DS_REPLACE,&tx)==DS_OK);
    CHECK(sys.ops->add(sys.ctx,tx,&d,&ref)==DS_STALE);sys.ops->abort(sys.ctx,tx);
    CHECK(app.ops->begin(app.ctx,DS_REPLACE,&tx)==DS_OK);
    CHECK(ds_core_register_image(&core,DS_APP,&port,&other)==DS_BUSY);
    CHECK(app.ops->background(app.ctx,tx,0x000000ff)==DS_OK);
    CHECK(app.ops->add(app.ctx,tx,&d,&ref)==DS_OK);
    CHECK(app.ops->end(app.ctx,tx)==DS_OK);
    ds_frame frame;CHECK(ds_core_frame(&core,&frame)==DS_OK);
    uint16_t rgb[2];uint8_t alpha[2];
    CHECK(ds_core_image_span(&core,frame.ticket,false,DS_APP,0,63,62,2,rgb,alpha)==DS_OK);
    CHECK(calls==1&&rgb[0]==18&&alpha[1]==255);
    CHECK(ds_core_image_span(&core,frame.ticket,false,DS_APP,0,64,0,1,rgb,alpha)==DS_INVALID);
    CHECK(ds_core_image_span(&core,frame.ticket,false,DS_APP,0,0,63,2,rgb,alpha)==DS_INVALID);
    CHECK(ds_core_image_span(&core,frame.ticket,false,DS_APP,0,0,0,1,NULL,alpha)==DS_INVALID);
    CHECK(calls==1);
    CHECK(ds_core_presented(&core,frame.ticket)==DS_OK);
    CHECK(ds_core_image_span(&core,frame.ticket,false,DS_APP,0,0,0,1,rgb,alpha)==DS_STALE);
    CHECK(app.ops->begin(app.ctx,DS_PATCH,&tx)==DS_OK);
    ds_change change={.property=DS_SET_IMAGE_FRAME,.value.image={2,0}};
    CHECK(app.ops->change(app.ctx,tx,ref,&change)==DS_INVALID);app.ops->abort(app.ctx,tx);
    CHECK(app.ops->begin(app.ctx,DS_PATCH,&tx)==DS_OK);
    change.value.image.variant=0;change.value.image.frame=0;
    CHECK(app.ops->change(app.ctx,tx,ref,&change)==DS_OK);CHECK(app.ops->end(app.ctx,tx)==DS_OK);
    CHECK(ds_core_frame(&core,&frame)==DS_OK);
    CHECK(ds_core_image_span(&core,frame.ticket,true,DS_APP,0,0,0,1,rgb,alpha)==DS_OK&&rgb[0]==18);
    CHECK(ds_core_image_span(&core,frame.ticket,false,DS_APP,0,0,0,1,rgb,alpha)==DS_OK&&rgb[0]==0);
    CHECK(ds_core_discard(&core,frame.ticket)==DS_OK);
    for(unsigned i=2;i<DS_RESOURCES;i++)CHECK(ds_core_register_image(&core,DS_APP,&port,&other)==DS_OK);
    CHECK(ds_core_register_image(&core,DS_APP,&port,&other)==DS_LIMIT);
    ds_core_init(&core);app=ds_core_client(&core,DS_APP);
    CHECK(ds_core_register_image(&core,DS_APP,&port,&other)==DS_OK&&other.value!=image.value);
    CHECK(app.ops->begin(app.ctx,DS_REPLACE,&tx)==DS_OK);
    CHECK(app.ops->add(app.ctx,tx,&d,&ref)==DS_STALE);app.ops->abort(app.ctx,tx);
    puts("image resources: PASS");return 0;
}
