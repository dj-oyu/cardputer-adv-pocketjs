/* White-box boundary test: no mutable test hooks in the production API. */
#include "../../main/ui/kasane/ksn_core.c"
#include <stdio.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"exhaustion line %d\n",__LINE__);return 1;}} while(0)
static ksn_result dummy_span(void *c,uint16_t v,uint16_t f,uint16_t y,uint16_t x,
                            uint16_t n,uint16_t *p,uint8_t *a){
    (void)c;(void)v;(void)f;(void)y;(void)x;(void)n;(void)p;(void)a;return KSN_OK;
}
int main(void){
    ksn_core core;ksn_core_init(&core);ksn_client app=ksn_core_client(&core,KSN_APP);
    ksn_tx tx;ksn_ref ref;
    last_generation=KSN_REF_GENERATION_MAX-1;
    CHECK(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    ksn_draw d={.kind=KSN_RECT,.bounds={0,0,2,2},.clip={0,0,2,2}};
    CHECK(app.ops->add(app.ctx,tx,&d,&ref)==KSN_OK);
    CHECK(ref_generation(ref)==KSN_REF_GENERATION_MAX);
    app.ops->abort(app.ctx,tx);
    CHECK(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_LIMIT);
    ksn_core_init(&core);app=ksn_core_client(&core,KSN_APP);
    CHECK(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_LIMIT);
    last_transaction=UINT32_MAX-1;
    CHECK(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK&&tx.value==UINT32_MAX);
    app.ops->abort(app.ctx,tx);
    CHECK(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_LIMIT);
    ksn_image_port port={NULL,1,1,1,1,dummy_span};ksn_resource id;
    last_resource=UINT32_MAX-1;
    CHECK(ksn_core_register_image(&core,KSN_APP,&port,&id)==KSN_OK&&id.value==UINT32_MAX);
    CHECK(ksn_core_register_image(&core,KSN_APP,&port,&id)==KSN_LIMIT);
    ksn_core_init(&core);
    CHECK(ksn_core_register_image(&core,KSN_APP,&port,&id)==KSN_LIMIT);
    ksn_core_init(&core);app=ksn_core_client(&core,KSN_APP);
    CHECK(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_LIMIT);
    puts("ID exhaustion: PASS");return 0;
}
