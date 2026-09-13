/* White-box boundary test: no mutable test hooks in the production API. */
#include "../../main/ui/ds/ds_core.c"
#include <stdio.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"exhaustion line %d\n",__LINE__);return 1;}} while(0)
static ds_result dummy_span(void *c,uint16_t v,uint16_t f,uint16_t y,uint16_t x,
                            uint16_t n,uint16_t *p,uint8_t *a){
    (void)c;(void)v;(void)f;(void)y;(void)x;(void)n;(void)p;(void)a;return DS_OK;
}
int main(void){
    ds_core core;ds_core_init(&core);ds_client app=ds_core_client(&core,DS_APP);
    ds_tx tx;ds_ref ref;
    last_generation=DS_REF_GENERATION_MAX-1;
    CHECK(app.ops->begin(app.ctx,DS_REPLACE,&tx)==DS_OK);
    ds_draw d={.kind=DS_RECT,.bounds={0,0,2,2},.clip={0,0,2,2}};
    CHECK(app.ops->add(app.ctx,tx,&d,&ref)==DS_OK);
    CHECK(ref_generation(ref)==DS_REF_GENERATION_MAX);
    app.ops->abort(app.ctx,tx);
    CHECK(app.ops->begin(app.ctx,DS_REPLACE,&tx)==DS_LIMIT);
    ds_core_init(&core);app=ds_core_client(&core,DS_APP);
    CHECK(app.ops->begin(app.ctx,DS_REPLACE,&tx)==DS_LIMIT);
    last_transaction=UINT32_MAX-1;
    CHECK(app.ops->begin(app.ctx,DS_PATCH,&tx)==DS_OK&&tx.value==UINT32_MAX);
    app.ops->abort(app.ctx,tx);
    CHECK(app.ops->begin(app.ctx,DS_PATCH,&tx)==DS_LIMIT);
    ds_image_port port={NULL,1,1,1,1,dummy_span};ds_resource id;
    last_resource=UINT32_MAX-1;
    CHECK(ds_core_register_image(&core,DS_APP,&port,&id)==DS_OK&&id.value==UINT32_MAX);
    CHECK(ds_core_register_image(&core,DS_APP,&port,&id)==DS_LIMIT);
    ds_core_init(&core);
    CHECK(ds_core_register_image(&core,DS_APP,&port,&id)==DS_LIMIT);
    ds_core_init(&core);app=ds_core_client(&core,DS_APP);
    CHECK(app.ops->begin(app.ctx,DS_PATCH,&tx)==DS_LIMIT);
    puts("ID exhaustion: PASS");return 0;
}
