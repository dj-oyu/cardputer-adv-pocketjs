#include "core_fixture.h"
#include <assert.h>
#include <stdio.h>
static ksn_result span(void *ctx,uint16_t v,uint16_t f,uint16_t y,uint16_t x,
                       uint16_t count,uint16_t *rgb,uint8_t *alpha){
    (void)ctx;(void)v;(void)f;(void)y;(void)x;
    for(unsigned i=0;i<count;i++){rgb[i]=123;alpha[i]=255;}
    return KSN_OK;
}
int main(void){
    KSN_TEST_CORE(core,static);ksn_core_init(&core);
    ksn_image_port port={NULL,16,12,1,1,span,true},got;
    ksn_resource id;assert(ksn_core_register_image(&core,KSN_APP,&port,&id)==KSN_OK);
    ksn_client app=ksn_core_client(&core,KSN_APP);ksn_tx tx;ksn_ref ref;
    ksn_draw d={.kind=KSN_IMAGE,.bounds={0,0,16,12},.clip={0,0,240,135},.opacity=255,
        .data.image={.resource=id,.scale=KSN_IMAGE_STRETCH,.source_width=16,.source_height=12}};
    assert(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    assert(app.ops->background(app.ctx,tx,0x000000ff)==KSN_OK);
    assert(ksn_core_unregister_image(&core,KSN_APP,id)==KSN_BUSY);
    assert(app.ops->add(app.ctx,tx,&d,&ref)==KSN_OK);
    assert(app.ops->end(app.ctx,tx)==KSN_OK);
    assert(ksn_core_unregister_image(&core,KSN_APP,id)==KSN_BUSY);
    assert(ksn_core_failed(&core,tx)==KSN_OK);
    assert(ksn_core_unregister_image(&core,KSN_APP,id)==KSN_BUSY);
    assert(ksn_core_presented(&core,tx)==KSN_OK);
    assert(ksn_core_unregister_image(&core,KSN_APP,id)==KSN_BUSY);
    assert(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    assert(app.ops->background(app.ctx,tx,0x000000ff)==KSN_OK);
    assert(app.ops->end(app.ctx,tx)==KSN_OK);
    assert(ksn_core_presented(&core,tx)==KSN_OK);
    assert(ksn_core_unregister_image(&core,KSN_APP,id)==KSN_OK);
    assert(ksn_core_image_port(&core,KSN_APP,id,&got)==KSN_STALE);
    assert(ksn_core_unregister_image(&core,KSN_APP,id)==KSN_STALE);
    uint32_t previous=id.value;
    for(unsigned i=0;i<600;i++){
        assert(ksn_core_register_image(&core,KSN_APP,&port,&id)==KSN_OK);
        assert(id.value>previous);previous=id.value;
        assert(ksn_core_unregister_image(&core,KSN_APP,id)==KSN_OK);
    }
    puts("image release: transaction/repair/reference guards, stale IDs, 600 cycles PASS");
}
