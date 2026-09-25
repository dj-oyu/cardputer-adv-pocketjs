#include "core_fixture.h"
#include "ksn_p0_probe.h"
#include "ksn_schema_session.h"
#include "ksn_source.h"
#include "ksn_source_pool_adapter.h"
#include "ksn_view_host.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define CHECK(x) do {if(!(x)){fprintf(stderr,"source copy line %d: %s\n",__LINE__,#x);return 1;}}while(0)
#define LIT KSN_SCHEMA_LITERAL
KSN_TEST_CORE(core,static);
KSN_TEST_CORE(pool_core,static);
KSN_TEST_CORE(shared_core,static);
static unsigned copy_calls[KSN_P0_COPY_COUNT];
static size_t copy_bytes[KSN_P0_COPY_COUNT];
static const char *watched_source_text;
static size_t watched_source_bytes;
static unsigned direct_source_calls;
static size_t direct_source_bytes;
void ksn_p0_probe_copy(ksn_p0_copy_kind kind,size_t bytes){
    if(bytes&&kind<KSN_P0_COPY_COUNT){copy_calls[kind]++;copy_bytes[kind]+=bytes;}
}
bool ksn_p0_probe_watch_source_text(const char *text,size_t bytes){
    watched_source_text=text;watched_source_bytes=bytes;return text&&bytes;
}
void ksn_p0_probe_unwatch_source_text(const char *text){
    if(text==watched_source_text)watched_source_text=NULL;
}
void ksn_p0_probe_core_source_text(const char *text,size_t bytes){
    if(text==watched_source_text&&bytes==watched_source_bytes){
        direct_source_calls++;direct_source_bytes+=bytes;
    }
}
typedef struct {
    char text[16];
    ksn_schema_value field;
    uint32_t generation;
    uint64_t revision;
    unsigned releases;
} producer;
static ksn_result acquire(void *context,uint64_t cursor,uint64_t now_us,
                          ksn_source_snapshot *out){
    producer *p=context;(void)cursor;(void)now_us;
    *out=(ksn_source_snapshot){.size=sizeof(*out),.version=KSN_SOURCE_ABI_VERSION,
        .field_count=1,.generation=p->generation,.revision=p->revision,
        .valid_fields=1,.changed_fields=1,.fields=&p->field};
    return KSN_OK;
}
static void release(void *context,const ksn_source_snapshot *snapshot){
    producer *p=context;
    if(snapshot->fields==&p->field)p->releases++;
}
static bool allow(void *context,uint32_t consumer){
    (void)context;return consumer==7;
}
typedef struct {uint16_t pixels[240*8];unsigned alpha,bravo,gamma,omega;} display;
static uint16_t *strip(void *context){return ((display *)context)->pixels;}
static ksn_result transfer(void *context,uint16_t y,uint16_t rows,const uint16_t *pixels){
    (void)context;(void)y;(void)rows;(void)pixels;return KSN_OK;
}
static ksn_result span(void *context,const ksn_draw *draw,uint16_t reveal,
                       int x,int y,unsigned count,uint8_t *out){
    display *d=context;(void)reveal;(void)x;(void)y;
    if(draw->data.text.bytes!=5)return KSN_INVALID;
    if(memcmp(draw->data.text.utf8,"alpha",5)==0){if(count)d->alpha++;}
    else if(memcmp(draw->data.text.utf8,"bravo",5)==0){if(count)d->bravo++;}
    else if(memcmp(draw->data.text.utf8,"gamma",5)==0){if(count)d->gamma++;}
    else if(memcmp(draw->data.text.utf8,"omega",5)==0){if(count)d->omega++;}
    else return KSN_INVALID;
    if(count)memset(out,255,count);
    return KSN_OK;
}
static int read_text(ksn_core *subject,ksn_tx tx,const char *expected){
    for(unsigned i=0;i<2;i++){
        ksn_frame_command command;
        if(ksn_core_read(subject,tx,false,KSN_APP,(uint16_t)i,&command)!=KSN_OK||
           command.draw.kind!=KSN_TEXT||command.draw.data.text.bytes!=5||
           memcmp(command.draw.data.text.utf8,expected,5)!=0)return 1;
    }
    return 0;
}
typedef struct {
    ksn_schema_value fields[1];
    char text[8];
} pool_payload;
static ksn_result pool_describe(const void *data,ksn_source_pool_view *out){
    const pool_payload *payload=data;
    *out=(ksn_source_pool_view){.fields=payload->fields,
        .valid_fields=1,.changed_fields=1};
    return KSN_OK;
}
static bool pool_allow(void *policy,uint32_t consumer){
    (void)policy;return consumer==7;
}
static ksn_result pool_publish(ksn_source_pool *pool,const char *text){
    ksn_source_write write={0};
    ksn_result result=ksn_source_pool_begin(pool,&write);
    if(result!=KSN_OK)return result;
    pool_payload *payload=write.data;
    memcpy(payload->text,text,6);
    payload->fields[0].data.text=(ksn_schema_text){payload->text,5};
    return ksn_source_pool_publish(&write,NULL);
}
static int pool_exhaustion_case(const ksn_schema *schema){
    pool_payload payloads[KSN_SOURCE_POOL_SLOTS]={0};
    ksn_source_pool pool={0};
    CHECK(ksn_source_pool_init(&pool,payloads,sizeof(payloads),sizeof(payloads[0]))==KSN_OK);
    static const ksn_slot_type types[]={KSN_SLOT_TEXT};
    ksn_source_pool_adapter adapter={0};ksn_source_provider provider={0};
    CHECK(ksn_source_pool_adapter_open(&adapter,&pool,types,1,
        offsetof(pool_payload,fields),pool_describe,pool_allow,NULL,&provider)==KSN_OK);
    ksn_source_registry registry;ksn_source_registry_init(&registry);
    ksn_source_handle handle={0};
    CHECK(ksn_source_register(&registry,&provider,&handle)==KSN_OK);
    CHECK(ksn_source_pool_adapter_registered(&adapter,handle)==KSN_OK);
    const ksn_source_binding binding={0,0};ksn_source_subscription sub={0};
    CHECK(ksn_source_subscribe(&registry,handle,7,schema,&binding,1,&sub)==KSN_OK);
    ksn_source_read pin_alpha={0},pin_bravo={0};
    CHECK(pool_publish(&pool,"alpha")==KSN_OK);
    CHECK(ksn_source_pool_acquire(&pool,&pin_alpha)==KSN_OK);
    CHECK(pool_publish(&pool,"bravo")==KSN_OK);
    CHECK(ksn_source_pool_acquire(&pool,&pin_bravo)==KSN_OK);
    CHECK(pool_publish(&pool,"gamma")==KSN_OK);
    CHECK(pool_publish(&pool,"delta")==KSN_BUSY&&ksn_source_pool_skipped(&pool)==1);
    CHECK(memcmp(((pool_payload *)pin_alpha.data)->text,"alpha",5)==0);
    CHECK(memcmp(((pool_payload *)pin_bravo.data)->text,"bravo",5)==0);

    ksn_schema_value base[2]={0},effective[KSN_SCHEMA_MAX_SLOTS]={0};
    base[0].data.text=(ksn_schema_text){"base",4};base[1].data.boolean=true;
    ksn_view_host host;ksn_view_host_init(&host,&pool_core,NULL,1);
    ksn_view *view=ksn_view_host_endpoint(&host,KSN_APP);
    ksn_schema_session session;CHECK(ksn_schema_session_init(&session,schema)==KSN_OK);
    display output={0};const ksn_text_port font={.ctx=&output,.span=span};
    ksn_display_port port={&output,strip,transfer,240,135,8,&font,NULL};
    ksn_rect viewport={0,0,240,135};ksn_render_stats stats;bool blocked=false;
    ksn_source_lease lease={0};
    CHECK(ksn_source_acquire(&registry,&sub,schema,base,1,effective,&lease)==KSN_OK);
    CHECK(memcmp(effective[0].data.text.utf8,"gamma",5)==0);
    CHECK(ksn_p0_probe_watch_source_text(effective[0].data.text.utf8,5));
    unsigned prior_calls=direct_source_calls;size_t prior_bytes=direct_source_bytes;
    CHECK(ksn_schema_session_step_dirty(&session,view,viewport,effective,3,1,&blocked)==KSN_OK&&blocked);
    CHECK(direct_source_calls==prior_calls+2&&direct_source_bytes==prior_bytes+10);
    CHECK(ksn_source_commit(&lease)==KSN_OK);ksn_source_release(&lease);
    CHECK(ksn_source_pool_release(&pin_alpha)==KSN_OK);
    CHECK(ksn_source_pool_release(&pin_bravo)==KSN_OK);
    CHECK(pool_publish(&pool,"omega")==KSN_OK);
    CHECK(!read_text(&pool_core,session.ticket,"gamma"));
    CHECK(ksn_view_host_present(&host,&port,&stats)==KSN_OK&&output.gamma>0);
    CHECK(ksn_source_presented(&sub,3)==KSN_OK);
    ksn_p0_probe_unwatch_source_text(watched_source_text);

    CHECK(ksn_source_acquire(&registry,&sub,schema,base,2,effective,&lease)==KSN_OK);
    CHECK(memcmp(effective[0].data.text.utf8,"omega",5)==0);
    CHECK(ksn_p0_probe_watch_source_text(effective[0].data.text.utf8,5));
    CHECK(ksn_schema_session_step_dirty(&session,view,viewport,effective,4,1,&blocked)==KSN_OK&&blocked);
    CHECK(direct_source_calls==prior_calls+4&&direct_source_bytes==prior_bytes+20);
    CHECK(ksn_source_commit(&lease)==KSN_OK);ksn_source_release(&lease);
    CHECK(!read_text(&pool_core,session.ticket,"omega"));
    CHECK(ksn_view_host_present(&host,&port,&stats)==KSN_OK&&output.omega>0);
    ksn_p0_probe_unwatch_source_text(watched_source_text);
    CHECK(ksn_source_unregister(&registry,handle)==KSN_OK);
    return 0;
}
int main(void){
    static const ksn_schema_slot slots[]={
        {"title",KSN_SLOT_TEXT,15,0,0},{"visible",KSN_SLOT_BOOL,0,0,0}};
    static const ksn_schema_node nodes[]={
        {.kind=KSN_NODE_TEXT,.bounds={.slot=LIT,.literal.rect={4,4,90,16}},
         .color={.slot=LIT,.literal.color=0xffffffffu},.text={.slot=0},
         .visible={.slot=1},.flags=KSN_SCHEMA_HAS_VISIBLE,.font=KSN_CAPTION},
        {.kind=KSN_NODE_TEXT,.bounds={.slot=LIT,.literal.rect={4,20,90,32}},
         .color={.slot=LIT,.literal.color=0xffffffffu},.text={.slot=0},
         .visible={.slot=1},.flags=KSN_SCHEMA_HAS_VISIBLE,.font=KSN_CAPTION}
    };
    const ksn_schema schema={.version=KSN_SCHEMA_ABI_VERSION,.slot_count=2,.node_count=2,
        .background=0x000000ffu,.slots=slots,.nodes=nodes};
    ksn_schema_value base[2]={0},effective[KSN_SCHEMA_MAX_SLOTS]={0};
    base[0].data.text=(ksn_schema_text){"base",4};
    base[1].data.boolean=true;
    producer p={.revision=1};memcpy(p.text,"alpha",6);
    p.field.data.text=(ksn_schema_text){p.text,5};
    CHECK(ksn_p0_probe_watch_source_text(p.text,5));
    static const ksn_slot_type types[]={KSN_SLOT_TEXT};
    ksn_source_provider provider={.size=sizeof(provider),.version=KSN_SOURCE_ABI_VERSION,
        .field_count=1,.field_types=types,.context=&p,
        .acquire=acquire,.release=release,.allow=allow};
    ksn_source_registry registry;ksn_source_registry_init(&registry);
    ksn_source_handle handle={0};
    CHECK(ksn_source_register(&registry,&provider,&handle)==KSN_OK);
    p.generation=handle.generation;
    const ksn_source_binding binding={0,0};ksn_source_subscription sub={0};
    CHECK(ksn_source_subscribe(&registry,handle,7,&schema,&binding,1,&sub)==KSN_OK);
    ksn_view_host host;ksn_view_host_init(&host,&core,NULL,1);
    ksn_view *view=ksn_view_host_endpoint(&host,KSN_APP);
    ksn_schema_session session;CHECK(ksn_schema_session_init(&session,&schema)==KSN_OK);
    display output={0};const ksn_text_port font={.ctx=&output,.span=span};
    ksn_display_port port={&output,strip,transfer,240,135,8,&font,NULL};
    ksn_rect viewport={0,0,240,135};ksn_render_stats stats;bool blocked=false;
    ksn_source_lease lease={0};
    CHECK(ksn_source_acquire(&registry,&sub,&schema,base,1,effective,&lease)==KSN_OK);
    CHECK(effective[0].data.text.utf8==p.text&&lease.dirty_slots==1);
    CHECK(ksn_schema_session_step_dirty(&session,view,viewport,effective,1,1,&blocked)==KSN_OK&&blocked);
    CHECK(copy_calls[KSN_P0_CORE_SUBMIT_TEXT]==2&&copy_bytes[KSN_P0_CORE_SUBMIT_TEXT]==10);
    CHECK(direct_source_calls==2&&direct_source_bytes==10);
    CHECK(copy_calls[KSN_P0_ADAPTER_TEMP]==0&&copy_calls[KSN_P0_ADAPTER_OWNED]==0);
    CHECK(ksn_source_commit(&lease)==KSN_OK);ksn_source_release(&lease);
    CHECK(p.releases==1);memcpy(p.text,"xxxxx",6);
    CHECK(!read_text(&core,session.ticket,"alpha"));
    CHECK(ksn_view_host_present(&host,&port,&stats)==KSN_OK&&output.alpha>0);
    CHECK(ksn_source_presented(&sub,1)==KSN_OK);

    p.revision=2;memcpy(p.text,"bravo",6);
    CHECK(ksn_source_acquire(&registry,&sub,&schema,base,2,effective,&lease)==KSN_OK);
    CHECK(effective[0].data.text.utf8==p.text&&lease.dirty_slots==1);
    CHECK(ksn_schema_session_step_dirty(&session,view,viewport,effective,2,1,&blocked)==KSN_OK&&blocked);
    CHECK(session.pending_delta==KSN_SCHEMA_PATCHED);
    CHECK(copy_calls[KSN_P0_CORE_SUBMIT_TEXT]==4&&copy_bytes[KSN_P0_CORE_SUBMIT_TEXT]==20);
    CHECK(copy_calls[KSN_P0_CORE_CLONE_TEXT]==0&&copy_bytes[KSN_P0_CORE_CLONE_TEXT]==0);
    CHECK(direct_source_calls==4&&direct_source_bytes==20);
    CHECK(ksn_source_commit(&lease)==KSN_OK);ksn_source_release(&lease);
    CHECK(p.releases==2);memcpy(p.text,"xxxxx",6);
    CHECK(!read_text(&core,session.ticket,"bravo"));
    CHECK(ksn_view_host_present(&host,&port,&stats)==KSN_OK&&output.bravo>0);
    CHECK(ksn_source_presented(&sub,2)==KSN_OK);
    ksn_view_host_invalidate(&host);
    CHECK(ksn_view_host_present(&host,&port,&stats)==KSN_OK);
    CHECK(copy_calls[KSN_P0_CORE_SUBMIT_TEXT]==4);

    p.revision=3;memcpy(p.text,"gamma",6);
    CHECK(ksn_source_acquire(&registry,&sub,&schema,base,3,effective,&lease)==KSN_OK);
    CHECK(lease.dirty_slots==1);
    CHECK(ksn_schema_session_step_dirty(&session,view,viewport,effective,3,1,&blocked)==KSN_OK&&blocked);
    CHECK(ksn_source_commit(&lease)==KSN_OK);ksn_source_release(&lease);
    CHECK(copy_calls[KSN_P0_CORE_SUBMIT_TEXT]==6);
    CHECK(direct_source_calls==6&&direct_source_bytes==30);
    CHECK(ksn_view_cancel(view,session.ticket)==KSN_OK);
    CHECK(ksn_source_acquire(&registry,&sub,&schema,base,4,effective,&lease)==KSN_OK);
    CHECK(lease.dirty_slots==0); /* Session must restore the discarded dirty bit. */
    CHECK(ksn_schema_session_step_dirty(&session,view,viewport,effective,3,0,&blocked)==KSN_OK&&blocked);
    CHECK(copy_calls[KSN_P0_CORE_SUBMIT_TEXT]==8&&copy_bytes[KSN_P0_CORE_SUBMIT_TEXT]==40);
    CHECK(direct_source_calls==8&&direct_source_bytes==40);
    CHECK(ksn_source_commit(&lease)==KSN_OK);ksn_source_release(&lease);
    memcpy(p.text,"xxxxx",6);
    CHECK(!read_text(&core,session.ticket,"gamma"));
    CHECK(ksn_view_host_present(&host,&port,&stats)==KSN_OK&&output.gamma>0);
    CHECK(ksn_source_presented(&sub,3)==KSN_OK);
    CHECK(copy_calls[KSN_P0_ADAPTER_TEMP]==0&&copy_calls[KSN_P0_ADAPTER_OWNED]==0);

    /* A new source value while hidden must not consume a core destination.
     * On visibility restoration, the newest immutable source value is copied
     * directly once per destination and survives lease release. */
    base[1].data.boolean=false;p.revision=4;memcpy(p.text,"delta",6);
    CHECK(ksn_source_acquire(&registry,&sub,&schema,base,5,effective,&lease)==KSN_OK);
    CHECK(effective[0].data.text.utf8==p.text&&lease.dirty_slots==1);
    CHECK(ksn_schema_session_step_dirty(&session,view,viewport,effective,4,3,&blocked)==KSN_OK&&blocked);
    CHECK(session.pending_delta==KSN_SCHEMA_REPLACED&&session.candidate_count==0);
    CHECK(copy_calls[KSN_P0_CORE_SUBMIT_TEXT]==8&&direct_source_calls==8);
    CHECK(ksn_source_commit(&lease)==KSN_OK);ksn_source_release(&lease);
    CHECK(ksn_view_host_present(&host,&port,&stats)==KSN_OK);
    CHECK(ksn_source_presented(&sub,4)==KSN_OK);

    base[1].data.boolean=true;p.revision=5;memcpy(p.text,"omega",6);
    CHECK(ksn_source_acquire(&registry,&sub,&schema,base,6,effective,&lease)==KSN_OK);
    CHECK(effective[0].data.text.utf8==p.text&&lease.dirty_slots==1);
    CHECK(ksn_schema_session_step_dirty(&session,view,viewport,effective,5,3,&blocked)==KSN_OK&&blocked);
    CHECK(session.pending_delta==KSN_SCHEMA_REPLACED&&session.candidate_count==2);
    CHECK(copy_calls[KSN_P0_CORE_SUBMIT_TEXT]==10&&copy_bytes[KSN_P0_CORE_SUBMIT_TEXT]==50);
    CHECK(direct_source_calls==10&&direct_source_bytes==50);
    CHECK(ksn_source_commit(&lease)==KSN_OK);ksn_source_release(&lease);
    memcpy(p.text,"xxxxx",6);
    CHECK(!read_text(&core,session.ticket,"omega"));
    CHECK(ksn_view_host_present(&host,&port,&stats)==KSN_OK&&output.omega>0);
    ksn_p0_probe_unwatch_source_text(p.text);
    CHECK(pool_exhaustion_case(&schema)==0);
    /* An unrelated layer's immutable text must cross neither bank on
     * REPLACE. Alternating APP/SYSTEM writers still render the same strings. */
    ksn_core_init(&shared_core);
    ksn_client shared_app=ksn_core_client(&shared_core,KSN_APP);
    ksn_client shared_sys=ksn_core_client(&shared_core,KSN_SYSTEM);
    ksn_tx shared_tx;ksn_ref shared_ref;
    ksn_draw shared_text={.kind=KSN_TEXT,.bounds={4,4,100,14},
        .clip={0,0,240,135},.opacity=255,
        .data.text={.utf8="alpha",.bytes=5,.capacity=8,.font=KSN_CAPTION,.color=0xffffffff}};
    CHECK(shared_app.ops->begin(shared_app.ctx,KSN_REPLACE,&shared_tx)==KSN_OK);
    CHECK(shared_app.ops->background(shared_app.ctx,shared_tx,0x000000ff)==KSN_OK);
    CHECK(shared_app.ops->add(shared_app.ctx,shared_tx,&shared_text,&shared_ref)==KSN_OK);
    CHECK(shared_app.ops->end(shared_app.ctx,shared_tx)==KSN_OK);
    CHECK(ksn_render_rects(&shared_core,&port,&stats)==KSN_OK);
    shared_text.bounds=(ksn_rect){4,24,100,34};shared_text.data.text.utf8="bravo";
    CHECK(shared_sys.ops->begin(shared_sys.ctx,KSN_REPLACE,&shared_tx)==KSN_OK);
    CHECK(shared_sys.ops->add(shared_sys.ctx,shared_tx,&shared_text,&shared_ref)==KSN_OK);
    CHECK(shared_sys.ops->end(shared_sys.ctx,shared_tx)==KSN_OK);
    CHECK(ksn_render_rects(&shared_core,&port,&stats)==KSN_OK);
    memset(copy_calls,0,sizeof(copy_calls));memset(copy_bytes,0,sizeof(copy_bytes));
    CHECK(shared_sys.ops->begin(shared_sys.ctx,KSN_PATCH,&shared_tx)==KSN_OK);
    CHECK(shared_sys.ops->end(shared_sys.ctx,shared_tx)==KSN_OK);
    CHECK(copy_calls[KSN_P0_CORE_CLONE_COMMAND]==0&&
          copy_bytes[KSN_P0_CORE_CLONE_COMMAND]==0);
    CHECK(ksn_render_rects(&shared_core,&port,&stats)==KSN_OK);
    ksn_change shared_color={.property=KSN_SET_COLOR,.value.color=0xff00ffff};
    CHECK(shared_sys.ops->begin(shared_sys.ctx,KSN_PATCH,&shared_tx)==KSN_OK);
    CHECK(shared_sys.ops->change(shared_sys.ctx,shared_tx,shared_ref,&shared_color)==KSN_OK);
    CHECK(shared_sys.ops->end(shared_sys.ctx,shared_tx)==KSN_OK);
    CHECK(copy_calls[KSN_P0_CORE_CLONE_COMMAND]==1&&
          copy_bytes[KSN_P0_CORE_CLONE_COMMAND]==sizeof(ksn_command_storage));
    CHECK(copy_calls[KSN_P0_CORE_CLONE_TEXT]==0&&copy_bytes[KSN_P0_CORE_CLONE_TEXT]==0);
    CHECK(ksn_render_rects(&shared_core,&port,&stats)==KSN_OK);
    shared_text.data.text.utf8="gamma";
    CHECK(shared_sys.ops->begin(shared_sys.ctx,KSN_REPLACE,&shared_tx)==KSN_OK);
    CHECK(shared_sys.ops->add(shared_sys.ctx,shared_tx,&shared_text,&shared_ref)==KSN_OK);
    CHECK(shared_sys.ops->end(shared_sys.ctx,shared_tx)==KSN_OK);
    CHECK(copy_calls[KSN_P0_CORE_CLONE_TEXT]==0&&copy_bytes[KSN_P0_CORE_CLONE_TEXT]==0);
    CHECK(ksn_render_rects(&shared_core,&port,&stats)==KSN_OK);
    shared_text.bounds=(ksn_rect){4,4,100,14};shared_text.data.text.utf8="omega";
    CHECK(shared_app.ops->begin(shared_app.ctx,KSN_REPLACE,&shared_tx)==KSN_OK);
    CHECK(shared_app.ops->background(shared_app.ctx,shared_tx,0x000000ff)==KSN_OK);
    CHECK(shared_app.ops->add(shared_app.ctx,shared_tx,&shared_text,&shared_ref)==KSN_OK);
    CHECK(shared_app.ops->end(shared_app.ctx,shared_tx)==KSN_OK);
    CHECK(copy_calls[KSN_P0_CORE_CLONE_TEXT]==0&&copy_bytes[KSN_P0_CORE_CLONE_TEXT]==0);
    CHECK(ksn_render_rects(&shared_core,&port,&stats)==KSN_OK);
    CHECK(output.gamma>0&&output.omega>0);
    puts("source copy: PASS (direct producer pointer, one copy/destination, release, PATCH, repair, discard/retry, hidden/show latest, pool exhaustion)");
    return 0;
}
