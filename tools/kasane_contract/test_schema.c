#include "core_fixture.h"
#include "ksn_schema.h"
#include "ksn_view_host.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"schema line %d: %s\n",__LINE__,#x);return 1;}}while(0)
#define LITERAL KSN_SCHEMA_LITERAL
#define BRECT(a,b,c,d) {.slot=LITERAL,.literal.rect={a,b,c,d}}
#define BCOLOR(c) {.slot=LITERAL,.literal.color=c}
#define BTEXT(s,n) {.slot=LITERAL,.literal.text={s,n}}
KSN_TEST_CORE(core,static);
static ksn_cache cache;
static ksn_cache_command_block cache_commands;
static ksn_cache_text_block cache_text;

int main(void){
    static const ksn_schema_slot slots[]={
        {"arbitraryCaption",KSN_SLOT_TEXT,47,0,0},
        {"arbitraryFill",KSN_SLOT_RECT,0,0,0},
        {"isShowing",KSN_SLOT_BOOL,0,0,0},
        {"screen",KSN_SLOT_U16,0,0,0}
    };
    static const ksn_schema_node nodes[]={
        {.kind=KSN_NODE_TEXT,.bounds=BRECT(8,8,220,22),.color=BCOLOR(0xe2f0ffffu),
         .text={.slot=0},.font=KSN_CAPTION},
        {.kind=KSN_NODE_RECT,.bounds={.slot=1},.color=BCOLOR(0x78c8ffffu),
         .visible={.slot=2},.flags=KSN_SCHEMA_HAS_VISIBLE},
        {.kind=KSN_NODE_PLATE_TEXT,.bounds=BRECT(8,28,9,44),
         .color=BCOLOR(0x96bedcffu),.plate_color=BCOLOR(0x000000ffu),
         .text=BTEXT("PAGE ONE",8),
         .page={.slot=3},.page_equals=1,.flags=KSN_SCHEMA_HAS_PAGE}
    };
    const ksn_schema schema={.version=1,.slot_count=4,.node_count=3,
                             .background=0x071425ffu,.slots=slots,.nodes=nodes};
    ksn_schema_value values[]={
        {.data.text={"ANY APP",7}},
        {.data.rect={12,109,54,111}},
        {.data.boolean=false},
        {.data.number=0}
    };
    CHECK(ksn_schema_validate(&schema)==KSN_OK);
    ksn_schema_dependencies dependencies;
    CHECK(ksn_schema_dependencies_build(&schema,&dependencies)==KSN_OK);
    CHECK(dependencies.nodes[0]==1u&&dependencies.nodes[1]==2u&&
          dependencies.nodes[2]==2u&&dependencies.nodes[3]==4u);
    CHECK(ksn_schema_values_validate(&schema,values)==KSN_OK);
    CHECK(ksn_schema_preflight(&schema,values,(ksn_rect){0,0,240,135})==KSN_OK);
    CHECK(ksn_schema_preflight(&schema,values,(ksn_rect){0,0,0,135})==KSN_INVALID);
    ksn_schema_node invalid_plate[3];memcpy(invalid_plate,nodes,sizeof(invalid_plate));
    invalid_plate[2].font=KSN_DISPLAY;
    ksn_schema invalid_font=schema;invalid_font.nodes=invalid_plate;
    CHECK(ksn_schema_validate(&invalid_font)==KSN_INVALID);
    values[0].data.text=(ksn_schema_text){"\xed\xa0\x80",3};
    CHECK(ksn_schema_values_validate(&schema,values)==KSN_INVALID);
    values[0].data.text=(ksn_schema_text){"ANY APP",7};
    values[1].data.rect=(ksn_rect){55,109,54,111};
    CHECK(ksn_schema_values_validate(&schema,values)==KSN_INVALID);
    values[1].data.rect=(ksn_rect){12,109,54,111};
    values[1].data.rect=(ksn_rect){12,109,12,111};
    CHECK(ksn_schema_values_validate(&schema,values)==KSN_OK);
    values[1].data.rect=(ksn_rect){12,109,54,111};
    CHECK(ksn_cache_bind(&cache,&cache_commands,&cache_text)==KSN_OK);
    ksn_view_host host;ksn_view_host_init(&host,&core,&cache,17);
    ksn_view *view=ksn_view_host_endpoint(&host,KSN_APP);
    ksn_ref refs[KSN_SCHEMA_MAX_NODES*2u]={0};uint8_t count=0;ksn_tx tx;
    CHECK(ksn_schema_submit(view,(ksn_rect){0,0,240,135},&schema,values,
                            refs,&count,&tx)==KSN_OK);
    CHECK(count==1);
    ksn_view_snapshot snapshot;
    CHECK(ksn_view_read_ref(view,refs[0],&snapshot)==KSN_STALE);
    ksn_frame frame;CHECK(ksn_core_frame(&core,&frame)==KSN_OK);
    CHECK(frame.next[KSN_APP].commands==1);
    CHECK(ksn_view_cancel(view,tx)==KSN_OK);
    values[1].data.rect=(ksn_rect){12,109,12,111};
    values[2].data.boolean=true;
    CHECK(ksn_schema_submit(view,(ksn_rect){0,0,240,135},&schema,values,
                            refs,&count,&tx)==KSN_OK&&count==1);
    CHECK(ksn_view_cancel(view,tx)==KSN_OK);
    values[1].data.rect=(ksn_rect){12,109,54,111};
    values[2].data.boolean=true;values[3].data.number=1;
    CHECK(ksn_schema_submit(view,(ksn_rect){0,0,240,135},&schema,values,
                            refs,&count,&tx)==KSN_OK);
    CHECK(count==4);
    CHECK(ksn_core_frame(&core,&frame)==KSN_OK);
    CHECK(frame.next[KSN_APP].commands==4);
    CHECK(ksn_core_presented(&core,tx)==KSN_OK);
    CHECK(ksn_view_read_ref(view,refs[0],&snapshot)==KSN_OK);
    CHECK(snapshot.draw.kind==KSN_TEXT&&snapshot.draw.data.text.bytes==7&&
          memcmp(snapshot.draw.data.text.utf8,"ANY APP",7)==0);
    ksn_view *system=ksn_view_host_endpoint(&host,KSN_SYSTEM);
    CHECK(ksn_view_read_ref(system,refs[0],&snapshot)==KSN_STALE);
    ksn_ref candidates[KSN_SCHEMA_MAX_NODES*2u]={0};uint8_t next_count=0;
    ksn_schema_delta delta=KSN_SCHEMA_REPLACED;
    CHECK(ksn_schema_update(view,(ksn_rect){0,0,240,135},&schema,values,refs,count,
                            schema.background,candidates,&next_count,&tx,&delta)==KSN_OK);
    CHECK(delta==KSN_SCHEMA_NO_CHANGE&&tx.value==0&&next_count==count);
    CHECK(ksn_schema_update(view,(ksn_rect){0,0,0,135},&schema,values,refs,count,
                            schema.background,candidates,&next_count,&tx,&delta)==KSN_INVALID);
    values[0].data.text=(ksn_schema_text){"ANOTHER APP",11};
    CHECK(ksn_schema_update(view,(ksn_rect){0,0,240,135},&schema,values,refs,count,
                            schema.background,candidates,&next_count,&tx,&delta)==KSN_OK);
    CHECK(delta==KSN_SCHEMA_PATCHED&&tx.value&&next_count==count);
    CHECK(ksn_core_frame(&core,&frame)==KSN_OK);
    CHECK(frame.next[KSN_APP].commands==4);
    CHECK(ksn_core_presented(&core,tx)==KSN_OK);
    CHECK(ksn_schema_update(view,(ksn_rect){0,0,240,135},&schema,values,refs,count,
                            schema.background,candidates,&next_count,&tx,&delta)==KSN_OK);
    CHECK(delta==KSN_SCHEMA_NO_CHANGE&&tx.value==0);
    values[1].data.rect=(ksn_rect){12,109,55,111};
    CHECK(ksn_schema_update(view,(ksn_rect){0,0,240,135},&schema,values,refs,count,
                            schema.background,candidates,&next_count,&tx,&delta)==KSN_OK);
    CHECK(delta==KSN_SCHEMA_PATCHED);
    CHECK(ksn_core_presented(&core,tx)==KSN_OK);
    values[2].data.boolean=false;values[3].data.number=0;
    CHECK(ksn_schema_update(view,(ksn_rect){0,0,240,135},&schema,values,refs,count,
                            schema.background,candidates,&next_count,&tx,&delta)==KSN_OK);
    CHECK(delta==KSN_SCHEMA_REPLACED&&next_count==1);
    CHECK(ksn_view_cancel(view,tx)==KSN_OK);
    ksn_schema_slot duplicate[4];memcpy(duplicate,slots,sizeof(duplicate));
    duplicate[1].name="arbitraryCaption";
    ksn_schema bad=schema;bad.slots=duplicate;
    CHECK(ksn_schema_validate(&bad)==KSN_INVALID);
    puts("schema: PASS");return 0;
}
