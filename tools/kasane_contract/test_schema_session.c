#include "core_fixture.h"
#include "ksn_schema_session.h"
#include "ksn_view_host.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"schema session line %d: %s\n",__LINE__,#x);return 1;}}while(0)
KSN_TEST_CORE(core,static);
static ksn_cache cache;
static ksn_cache_command_block cache_commands;
static ksn_cache_text_block cache_text;
static uint16_t strip[240*8],panel[240*135];
static uint16_t *buffer(void *ctx){(void)ctx;return strip;}
static ksn_result transfer(void *ctx,uint16_t y,uint16_t rows,const uint16_t *p){
    (void)ctx;memcpy(panel+y*240,p,rows*240*sizeof(*p));return KSN_OK;
}
int main(void){
    static const ksn_schema_slot slots[]={{"fill",KSN_SLOT_RECT,0,0,0}};
    static const ksn_schema_node nodes[]={{
        .kind=KSN_NODE_RECT,.bounds={.slot=0},
        .color={.slot=KSN_SCHEMA_LITERAL,.literal.color=0x78c8ffffu}
    }};
    const ksn_schema schema={.version=KSN_SCHEMA_ABI_VERSION,.slot_count=1,.node_count=1,
                             .background=0x000000ffu,.slots=slots,.nodes=nodes};
    ksn_schema_value values[]={{.data.rect={12,109,12,111}}};
    ksn_schema_session session;
    CHECK(ksn_schema_session_init(&session,&schema)==KSN_OK);
    CHECK(sizeof(session)<1280);
    CHECK(ksn_cache_bind(&cache,&cache_commands,&cache_text)==KSN_OK);
    ksn_view_host host;ksn_view_host_init(&host,&core,&cache,17);
    ksn_view *view=ksn_view_host_endpoint(&host,KSN_APP);
    ksn_display_port port={NULL,buffer,transfer,240,135,8,NULL,NULL};
    ksn_render_stats stats;bool blocked=false;
    ksn_rect viewport={0,0,240,135};
    CHECK(ksn_schema_session_step(&session,view,viewport,values,1,&blocked)==KSN_OK&&blocked);
    CHECK(!session.has_active&&session.candidate_count==0);
    CHECK(ksn_schema_session_settle(&session,view,&blocked)==KSN_OK&&blocked);
    CHECK(session.ticket.value&&!session.has_active);
    CHECK(ksn_view_host_present(&host,&port,&stats)==KSN_OK);
    CHECK(ksn_schema_session_settle(&session,view,&blocked)==KSN_OK&&!blocked);
    CHECK(!session.ticket.value&&session.has_active&&session.applied_revision==1);
    CHECK(ksn_schema_session_step(&session,view,viewport,values,1,&blocked)==KSN_OK&&!blocked);
    CHECK(session.has_active&&session.active_count==0);
    values[0].data.rect=(ksn_rect){12,109,32,111};
    CHECK(ksn_schema_session_step(&session,view,viewport,values,2,&blocked)==KSN_OK&&blocked);
    CHECK(session.candidate_count==1);
    values[0].data.rect=(ksn_rect){12,109,42,111};
    CHECK(ksn_schema_session_step(&session,view,viewport,values,3,&blocked)==KSN_OK&&blocked);
    CHECK(ksn_view_host_present(&host,&port,&stats)==KSN_OK);
    CHECK(ksn_schema_session_step(&session,view,viewport,values,3,&blocked)==KSN_OK&&blocked);
    CHECK(session.pending_delta==KSN_SCHEMA_PATCHED);
    CHECK(ksn_view_host_present(&host,&port,&stats)==KSN_OK);
    CHECK(ksn_schema_session_step(&session,view,viewport,values,3,&blocked)==KSN_OK&&!blocked);
    CHECK(session.active_count==1&&session.applied_revision==3);
    ksn_view_snapshot snapshot;
    CHECK(ksn_view_read_ref(view,session.active_refs[0],&snapshot)==KSN_OK);
    CHECK(snapshot.draw.bounds.x1==42);
    values[0].data.rect=(ksn_rect){12,109,12,111};
    CHECK(ksn_schema_session_step(&session,view,viewport,values,4,&blocked)==KSN_OK&&blocked);
    CHECK(session.pending_delta==KSN_SCHEMA_REPLACED&&session.candidate_count==0);
    CHECK(ksn_view_cancel(view,session.ticket)==KSN_OK);
    CHECK(ksn_schema_session_settle(&session,view,&blocked)==KSN_OK&&!blocked);
    CHECK(!session.ticket.value&&session.dirty_unknown&&session.active_count==1);
    CHECK(ksn_schema_session_step(&session,view,viewport,values,4,&blocked)==KSN_OK&&blocked);
    CHECK(session.active_count==1);
    CHECK(ksn_view_read_ref(view,session.active_refs[0],&snapshot)==KSN_OK);
    CHECK(ksn_view_host_present(&host,&port,&stats)==KSN_OK);
    CHECK(ksn_schema_session_step(&session,view,viewport,values,4,&blocked)==KSN_OK&&!blocked);
    CHECK(session.active_count==0&&session.applied_revision==4);
    printf("schema session: PASS (%zu bytes)\n",sizeof(session));return 0;
}
