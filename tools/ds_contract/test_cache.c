#include "ds_cache.h"
#include "ds_render.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"cache line %d: %s\n",__LINE__,#x);return 1;}} while(0)

static uint16_t panel[240*135],strip[240*8];
static uint16_t *get_strip(void *ctx){(void)ctx;return strip;}
static ds_result present(void *ctx,uint16_t y,uint16_t rows,const uint16_t *pixels){
    (void)ctx;memcpy(panel+y*240,pixels,rows*240*sizeof(uint16_t));return DS_OK;
}
static unsigned colored_pixels(void){
    unsigned count=0;for(unsigned i=0;i<240*135;i++)if(panel[i])count++;return count;
}
static ds_result render_and_resolve(ds_cache *cache,ds_core *core,const ds_display_port *display){
    ds_frame frame;ds_result result=ds_core_frame(core,&frame);if(result!=DS_OK)return result;
    ds_render_stats stats;result=ds_render_rects(core,display,&stats);
    if(result!=DS_OK)return result;
    return ds_cache_resolve(cache,core,frame.ticket,true);
}
int main(void){
    ds_core core;ds_cache cache;ds_core_init(&core);ds_cache_init(&cache);
    ds_client app=ds_core_client(&core,DS_APP);
    ds_display_port display={NULL,get_strip,present,240,135,8};
    ds_draw card[2]={
        {.kind=DS_RECT,.bounds={0,0,40,24},.clip={0,0,40,24},.opacity=255,
         .data.shape={0x1c3043ff,0,0}},
        {.kind=DS_RECT,.bounds={8,8,32,20},.clip={0,0,40,24},.opacity=255,
         .data.shape={0x67dfc7ff,0,0}}
    };
    ds_template first,second;CHECK(ds_cache_create(&cache,DS_APP,card,2,&first)==DS_OK);
    card[0].data.shape.color=0xf5bb69ff;
    CHECK(ds_cache_create(&cache,DS_APP,card,2,&second)==DS_OK);
    ds_cache_stats usage=ds_cache_get_stats(&cache);
    CHECK(usage.native_bytes==DS_CACHE_STORAGE_BYTES+2*sizeof(uint32_t));
    CHECK(usage.templates==2&&usage.commands==4&&usage.text_bytes==0);
    ds_placement left={8,8,{0,0,240,135},255,true},right={128,8,{0,0,240,135},255,true};
    ds_tx tx;ds_instance a,b;
    CHECK(app.ops->begin(app.ctx,DS_REPLACE,&tx)==DS_OK);
    CHECK(app.ops->background(app.ctx,tx,0x000000ff)==DS_OK);
    CHECK(ds_cache_instantiate(&cache,&core,tx,first,&left,&a)==DS_OK);
    CHECK(ds_cache_instantiate(&cache,&core,tx,first,&right,&b)==DS_OK);
    CHECK(ds_cache_release(&cache,first)==DS_BUSY);
    CHECK(app.ops->end(app.ctx,tx)==DS_OK);
    CHECK(render_and_resolve(&cache,&core,&display)==DS_OK);
    CHECK(colored_pixels()==2u*(40u*24u));
    usage=ds_cache_get_stats(&cache);CHECK(usage.instances==2);

    /* Aborting an instance creation releases its provisional registry slot. */
    CHECK(app.ops->begin(app.ctx,DS_REPLACE,&tx)==DS_OK);
    CHECK(app.ops->background(app.ctx,tx,0x000000ff)==DS_OK);
    ds_instance provisional;
    CHECK(ds_cache_instantiate(&cache,&core,tx,second,&left,&provisional)==DS_OK);
    app.ops->abort(app.ctx,tx);CHECK(ds_cache_abort(&cache,tx)==DS_OK);
    CHECK(ds_cache_get_stats(&cache).instances==2);
    CHECK(ds_cache_set_visible(&cache,&core,tx,provisional,false)==DS_STALE);

    /* One shared template, independently moved and hidden. */
    CHECK(app.ops->begin(app.ctx,DS_PATCH,&tx)==DS_OK);
    left.x=48;left.y=64;
    CHECK(ds_cache_place(&cache,&core,tx,a,&left)==DS_OK);
    CHECK(ds_cache_set_visible(&cache,&core,tx,b,false)==DS_OK);
    CHECK(app.ops->end(app.ctx,tx)==DS_OK);
    CHECK(render_and_resolve(&cache,&core,&display)==DS_OK);
    CHECK(colored_pixels()==40u*24u);

    /* A discarded placement leaves the committed placement usable. */
    CHECK(app.ops->begin(app.ctx,DS_PATCH,&tx)==DS_OK);left.x=190;
    CHECK(ds_cache_place(&cache,&core,tx,a,&left)==DS_OK);CHECK(app.ops->end(app.ctx,tx)==DS_OK);
    ds_frame frame;CHECK(ds_core_frame(&core,&frame)==DS_OK);
    CHECK(ds_core_discard(&core,frame.ticket)==DS_OK);
    CHECK(ds_cache_resolve(&cache,&core,frame.ticket,false)==DS_OK);
    CHECK(app.ops->begin(app.ctx,DS_PATCH,&tx)==DS_OK);
    CHECK(ds_cache_set_visible(&cache,&core,tx,a,false)==DS_OK);CHECK(app.ops->end(app.ctx,tx)==DS_OK);
    CHECK(render_and_resolve(&cache,&core,&display)==DS_OK);CHECK(colored_pixels()==0);

    /* Omitting instances in a presented REPLACE detaches them. */
    CHECK(app.ops->begin(app.ctx,DS_REPLACE,&tx)==DS_OK);
    CHECK(app.ops->background(app.ctx,tx,0x000000ff)==DS_OK);CHECK(app.ops->end(app.ctx,tx)==DS_OK);
    CHECK(render_and_resolve(&cache,&core,&display)==DS_OK);
    usage=ds_cache_get_stats(&cache);CHECK(usage.instances==0);
    CHECK(ds_cache_release(&cache,first)==DS_OK);
    CHECK(ds_cache_release(&cache,first)==DS_STALE);
    CHECK(ds_cache_release(&cache,second)==DS_OK);
    usage=ds_cache_get_stats(&cache);CHECK(usage.templates==0&&usage.commands==0);

    /* Unsupported group opacity is rejected before touching the transaction. */
    CHECK(ds_cache_create(&cache,DS_APP,card,2,&first)==DS_OK);
    CHECK(app.ops->begin(app.ctx,DS_REPLACE,&tx)==DS_OK);
    CHECK(app.ops->background(app.ctx,tx,0x000000ff)==DS_OK);left.opacity=128;
    CHECK(ds_cache_instantiate(&cache,&core,tx,first,&left,&a)==DS_UNSUPPORTED);
    left.opacity=255;CHECK(ds_cache_instantiate(&cache,&core,tx,first,&left,&a)==DS_OK);
    CHECK(app.ops->end(app.ctx,tx)==DS_OK);CHECK(render_and_resolve(&cache,&core,&display)==DS_OK);
    puts("explicit component cache: PASS");return 0;
}
