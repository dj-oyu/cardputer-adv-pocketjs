/* White-box boundary test: no mutable test hooks in the production API. */
#include "../../main/ui/ds/ds_core.c"
#include <stdio.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"exhaustion line %d\n",__LINE__);return 1;}} while(0)
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
    ds_core_init(&core);app=ds_core_client(&core,DS_APP);
    CHECK(app.ops->begin(app.ctx,DS_PATCH,&tx)==DS_LIMIT);
    puts("ID exhaustion: PASS");return 0;
}
