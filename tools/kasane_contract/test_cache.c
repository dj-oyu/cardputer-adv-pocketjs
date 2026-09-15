#include "core_fixture.h"
#include "ksn_cache.h"
#include "ksn_render.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"cache line %d: %s\n",__LINE__,#x);return 1;}} while(0)

static uint16_t panel[240*135],strip[240*8];
static uint16_t *get_strip(void *ctx){(void)ctx;return strip;}
static ksn_result present(void *ctx,uint16_t y,uint16_t rows,const uint16_t *pixels){
    (void)ctx;memcpy(panel+y*240,pixels,rows*240*sizeof(uint16_t));return KSN_OK;
}
static unsigned colored_pixels(void){
    unsigned count=0;for(unsigned i=0;i<240*135;i++)if(panel[i])count++;return count;
}
static ksn_result render_and_resolve(ksn_cache *cache,ksn_core *core,const ksn_display_port *display){
    ksn_frame frame;ksn_result result=ksn_core_frame(core,&frame);if(result!=KSN_OK)return result;
    ksn_render_stats stats;result=ksn_render_rects(core,display,&stats);
    if(result!=KSN_OK)return result;
    return ksn_cache_resolve(cache,core,frame.ticket,true);
}
int main(void){
    KSN_TEST_CORE(core,);ksn_cache cache;ksn_cache_command_block commands;ksn_cache_text_block text;
    ksn_core_init(&core);CHECK(ksn_cache_bind(&cache,&commands,&text)==KSN_OK);
    ksn_client app=ksn_core_client(&core,KSN_APP);
    ksn_display_port display={NULL,get_strip,present,240,135,8};
    ksn_draw card[2]={
        {.kind=KSN_RECT,.bounds={0,0,40,24},.clip={0,0,40,24},.opacity=255,
         .data.shape={0x1c3043ff,0,0}},
        {.kind=KSN_RECT,.bounds={8,8,32,20},.clip={0,0,40,24},.opacity=255,
         .data.shape={0x67dfc7ff,0,0}}
    };
    ksn_template first,second;CHECK(ksn_cache_create(&cache,KSN_APP,card,2,&first)==KSN_OK);
    card[0].data.shape.color=0xf5bb69ff;
    CHECK(ksn_cache_create(&cache,KSN_APP,card,2,&second)==KSN_OK);
    ksn_cache_stats usage=ksn_cache_get_stats(&cache);
    CHECK(usage.native_bytes==KSN_CACHE_RESERVED_BYTES+2*sizeof(uint32_t));
    CHECK(usage.templates==2&&usage.commands==4&&usage.text_bytes==0);
    ksn_placement left={8,8,{0,0,240,135},255,true},right={128,8,{0,0,240,135},255,true};
    ksn_tx tx;ksn_instance a,b;
    CHECK(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    CHECK(app.ops->background(app.ctx,tx,0x000000ff)==KSN_OK);
    CHECK(ksn_cache_instantiate(&cache,&core,tx,first,&left,&a)==KSN_OK);
    CHECK(ksn_cache_instantiate(&cache,&core,tx,first,&right,&b)==KSN_OK);
    CHECK(ksn_cache_release(&cache,first)==KSN_BUSY);
    CHECK(app.ops->end(app.ctx,tx)==KSN_OK);
    CHECK(render_and_resolve(&cache,&core,&display)==KSN_OK);
    CHECK(colored_pixels()==2u*(40u*24u));
    usage=ksn_cache_get_stats(&cache);CHECK(usage.instances==2);

    /* Aborting an instance creation releases its provisional registry slot. */
    CHECK(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    CHECK(app.ops->background(app.ctx,tx,0x000000ff)==KSN_OK);
    ksn_instance provisional;
    CHECK(ksn_cache_instantiate(&cache,&core,tx,second,&left,&provisional)==KSN_OK);
    app.ops->abort(app.ctx,tx);CHECK(ksn_cache_abort(&cache,tx)==KSN_OK);
    CHECK(ksn_cache_get_stats(&cache).instances==2);
    CHECK(ksn_cache_set_visible(&cache,&core,tx,provisional,false)==KSN_STALE);

    /* One shared template, independently moved and hidden. */
    CHECK(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
    left.x=48;left.y=64;
    CHECK(ksn_cache_place(&cache,&core,tx,a,&left)==KSN_OK);
    CHECK(ksn_cache_set_visible(&cache,&core,tx,b,false)==KSN_OK);
    CHECK(app.ops->end(app.ctx,tx)==KSN_OK);
    CHECK(render_and_resolve(&cache,&core,&display)==KSN_OK);
    CHECK(colored_pixels()==40u*24u);

    /* A discarded placement leaves the committed placement usable. */
    CHECK(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);left.x=190;
    CHECK(ksn_cache_place(&cache,&core,tx,a,&left)==KSN_OK);CHECK(app.ops->end(app.ctx,tx)==KSN_OK);
    ksn_frame frame;CHECK(ksn_core_frame(&core,&frame)==KSN_OK);
    CHECK(ksn_core_discard(&core,frame.ticket)==KSN_OK);
    CHECK(ksn_cache_resolve(&cache,&core,frame.ticket,false)==KSN_OK);
    CHECK(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
    CHECK(ksn_cache_set_visible(&cache,&core,tx,a,false)==KSN_OK);CHECK(app.ops->end(app.ctx,tx)==KSN_OK);
    CHECK(render_and_resolve(&cache,&core,&display)==KSN_OK);CHECK(colored_pixels()==0);

    /* Omitting instances in a presented REPLACE detaches them. */
    CHECK(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    CHECK(app.ops->background(app.ctx,tx,0x000000ff)==KSN_OK);CHECK(app.ops->end(app.ctx,tx)==KSN_OK);
    CHECK(render_and_resolve(&cache,&core,&display)==KSN_OK);
    usage=ksn_cache_get_stats(&cache);CHECK(usage.instances==0);
    CHECK(ksn_cache_release(&cache,first)==KSN_OK);
    CHECK(ksn_cache_release(&cache,first)==KSN_STALE);
    CHECK(ksn_cache_release(&cache,second)==KSN_OK);
    usage=ksn_cache_get_stats(&cache);CHECK(usage.templates==0&&usage.commands==0);

    /* Group opacity is accepted and can be changed before submission. */
    CHECK(ksn_cache_create(&cache,KSN_APP,card,2,&first)==KSN_OK);
    CHECK(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    CHECK(app.ops->background(app.ctx,tx,0x000000ff)==KSN_OK);left.opacity=128;
    CHECK(ksn_cache_instantiate(&cache,&core,tx,first,&left,&a)==KSN_OK);
    left.opacity=255;CHECK(ksn_cache_place(&cache,&core,tx,a,&left)==KSN_OK);
    CHECK(app.ops->end(app.ctx,tx)==KSN_OK);CHECK(render_and_resolve(&cache,&core,&display)==KSN_OK);
    puts("explicit component cache: PASS");return 0;
}
