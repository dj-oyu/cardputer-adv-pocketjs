#include "core_fixture.h"
#include "ksn_core.h"
#include <stdio.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"resource line %d: %s\n",__LINE__,#x);return 1;}} while(0)
static unsigned calls;
static ksn_result span(void *ctx,uint16_t variant,uint16_t frame,uint16_t y,uint16_t x,
                       uint16_t count,uint16_t *rgb,uint8_t *alpha){
    (void)ctx;(void)y;(void)x;calls++;
    for(unsigned i=0;i<count;i++){rgb[i]=(uint16_t)(variant*16+frame);alpha[i]=255;}
    return KSN_OK;
}
int main(void){
    KSN_TEST_CORE(core,);ksn_core_init(&core);ksn_client app=ksn_core_client(&core,KSN_APP),sys=ksn_core_client(&core,KSN_SYSTEM);
    ksn_image_port port={NULL,64,64,2,3,span};ksn_resource image,other;
    CHECK(ksn_core_register_image(&core,KSN_APP,&port,&image)==KSN_OK);
    port.width=1; /* The descriptor was copied. */
    CHECK(ksn_core_register_image(&core,KSN_SYSTEM,&port,&other)==KSN_OK);
    ksn_tx tx;ksn_ref ref;
    ksn_draw d={.kind=KSN_IMAGE,.bounds={0,0,64,64},.clip={0,0,240,135},.opacity=255};
    d.data.image.resource=image;d.data.image.variant=1;d.data.image.frame=2;
    CHECK(sys.ops->begin(sys.ctx,KSN_REPLACE,&tx)==KSN_OK);
    CHECK(sys.ops->add(sys.ctx,tx,&d,&ref)==KSN_STALE);sys.ops->abort(sys.ctx,tx);
    CHECK(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    CHECK(ksn_core_register_image(&core,KSN_APP,&port,&other)==KSN_BUSY);
    CHECK(app.ops->background(app.ctx,tx,0x000000ff)==KSN_OK);
    CHECK(app.ops->add(app.ctx,tx,&d,&ref)==KSN_OK);
    CHECK(app.ops->end(app.ctx,tx)==KSN_OK);
    ksn_frame frame;CHECK(ksn_core_frame(&core,&frame)==KSN_OK);
    uint16_t rgb[2];uint8_t alpha[2];
    CHECK(ksn_core_image_span(&core,frame.ticket,false,KSN_APP,0,63,62,2,rgb,alpha)==KSN_OK);
    CHECK(calls==1&&rgb[0]==18&&alpha[1]==255);
    CHECK(ksn_core_image_span(&core,frame.ticket,false,KSN_APP,0,64,0,1,rgb,alpha)==KSN_INVALID);
    CHECK(ksn_core_image_span(&core,frame.ticket,false,KSN_APP,0,0,63,2,rgb,alpha)==KSN_INVALID);
    CHECK(ksn_core_image_span(&core,frame.ticket,false,KSN_APP,0,0,0,1,NULL,alpha)==KSN_INVALID);
    CHECK(calls==1);
    CHECK(ksn_core_presented(&core,frame.ticket)==KSN_OK);
    CHECK(ksn_core_image_span(&core,frame.ticket,false,KSN_APP,0,0,0,1,rgb,alpha)==KSN_STALE);
    CHECK(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
    ksn_change change={.property=KSN_SET_IMAGE_FRAME,.value.image={2,0}};
    CHECK(app.ops->change(app.ctx,tx,ref,&change)==KSN_INVALID);app.ops->abort(app.ctx,tx);
    CHECK(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
    change.value.image.variant=0;change.value.image.frame=0;
    CHECK(app.ops->change(app.ctx,tx,ref,&change)==KSN_OK);CHECK(app.ops->end(app.ctx,tx)==KSN_OK);
    CHECK(ksn_core_frame(&core,&frame)==KSN_OK);
    CHECK(ksn_core_image_span(&core,frame.ticket,true,KSN_APP,0,0,0,1,rgb,alpha)==KSN_OK&&rgb[0]==18);
    CHECK(ksn_core_image_span(&core,frame.ticket,false,KSN_APP,0,0,0,1,rgb,alpha)==KSN_OK&&rgb[0]==0);
    CHECK(ksn_core_discard(&core,frame.ticket)==KSN_OK);
    for(unsigned i=2;i<KSN_RESOURCES;i++)CHECK(ksn_core_register_image(&core,KSN_APP,&port,&other)==KSN_OK);
    CHECK(ksn_core_register_image(&core,KSN_APP,&port,&other)==KSN_LIMIT);
    ksn_core_init(&core);app=ksn_core_client(&core,KSN_APP);
    CHECK(ksn_core_register_image(&core,KSN_APP,&port,&other)==KSN_OK&&other.value!=image.value);
    CHECK(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    CHECK(app.ops->add(app.ctx,tx,&d,&ref)==KSN_STALE);app.ops->abort(app.ctx,tx);
    puts("image resources: PASS");return 0;
}
