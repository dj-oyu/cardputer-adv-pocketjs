#include "app_view_assets.h"
#include "pocket_clock_source.h"
#include "pet/ksn_pet.h"
#include <string.h>

#define LIT KSN_SCHEMA_LITERAL
#define RECT(a,b,c,d) {.slot=LIT,.literal.rect={a,b,c,d}}
#define COLOR(c) {.slot=LIT,.literal.color=c}
#define TEXT(s) {.slot=LIT,.literal.text={s,(uint16_t)(sizeof(s)-1u)}}
static const ksn_schema_slot hello_slots[]={
    {"counter",KSN_SLOT_TEXT,KSN_SCHEMA_TEXT_MAX,0,0}
};
static const ksn_schema_node hello_nodes[]={
    {.kind=KSN_NODE_TEXT,.bounds=RECT(16,12,226,22),.color=COLOR(0x69cdeeffu),
     .text=TEXT("KASANE / JAVASCRIPT"),.font=KSN_CAPTION},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(16,34,236,52),.color=COLOR(0xf0f8ffffu),
     .text=TEXT("Hello, World!"),.font=KSN_DISPLAY},
    {.kind=KSN_NODE_ROUND_RECT,.bounds=RECT(16,62,224,100),
     .color=COLOR(0x12334affu),.radius=6},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(28,77,212,89),.color=COLOR(0x8ef0c4ffu),
     .text={.slot=0},.font=KSN_CAPTION},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(16,117,236,127),.color=COLOR(0xa9bacaffu),
     .text=TEXT("ENTER +1    ESC HOME"),.font=KSN_CAPTION}
};
static const ksn_schema hello={.version=1,.slot_count=1,.node_count=5,
    .background=0x071425ffu,.slots=hello_slots,.nodes=hello_nodes};

static const ksn_schema_slot imucal_slots[]={
    {"head",KSN_SLOT_TEXT,47,0,0},{"live",KSN_SLOT_TEXT,47,0,0},
    {"stat",KSN_SLOT_TEXT,47,0,0},{"spin",KSN_SLOT_TEXT,47,0,0},
    {"foot",KSN_SLOT_TEXT,47,0,0}
};
static const ksn_schema_node imucal_nodes[]={
    {.kind=KSN_NODE_TEXT,.bounds=RECT(12,8,228,20),.color=COLOR(0x69cdeeffu),
     .text=TEXT("IMU AXIS CALIBRATION"),.font=KSN_BODY},
    {.kind=KSN_NODE_ROUND_RECT,.bounds=RECT(12,48,228,94),
     .color=COLOR(0x12334affu),.radius=5},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(12,26,228,38),.color=COLOR(0xf0f8ffffu),
     .text={.slot=0},.font=KSN_BODY},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(20,52,220,64),.color=COLOR(0x8ef0c4ffu),
     .text={.slot=1},.font=KSN_BODY},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(20,66,220,78),.color=COLOR(0xa9bacaffu),
     .text={.slot=2},.font=KSN_BODY},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(20,80,220,92),.color=COLOR(0xffd479ffu),
     .text={.slot=3},.font=KSN_BODY},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(12,104,228,116),.color=COLOR(0x8fa6bcffu),
     .text={.slot=4},.font=KSN_BODY}
};
static const ksn_schema imucal={.version=1,.slot_count=5,.node_count=7,
    .background=0x071425ffu,.slots=imucal_slots,.nodes=imucal_nodes};

static const ksn_schema_slot bridge_slots[]={
    {"st",KSN_SLOT_TEXT,47,0,0},{"info",KSN_SLOT_TEXT,47,0,0},
    {"job",KSN_SLOT_TEXT,47,0,0},{"seen",KSN_SLOT_TEXT,47,0,0}
};
static const ksn_schema_node bridge_nodes[]={
    {.kind=KSN_NODE_TEXT,.bounds=RECT(10,8,230,18),.color=COLOR(0x7fd7ffffu),
     .text=TEXT("POCKET BRIDGE / PC LINK"),.font=KSN_CAPTION},
    {.kind=KSN_NODE_RECT,.bounds=RECT(8,24,232,98),.color=COLOR(0x14293cffu)},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(16,31,224,41),.color=COLOR(0xf5eedcffu),
     .text={.slot=0},.font=KSN_CAPTION},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(16,49,224,59),.color=COLOR(0x9fb6c8ffu),
     .text={.slot=1},.font=KSN_CAPTION},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(16,66,224,76),.color=COLOR(0x9fb6c8ffu),
     .text={.slot=2},.font=KSN_CAPTION},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(16,83,224,93),.color=COLOR(0x8ef0c4ffu),
     .text={.slot=3},.font=KSN_CAPTION},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(10,120,230,129),.color=COLOR(0xf5bb69ffu),
     .text=TEXT("ENTER SUBMIT   ESC HOME"),.font=KSN_CAPTION}
};
static const ksn_schema bridge={.version=1,.slot_count=4,.node_count=7,
    .background=0x08131fffu,.slots=bridge_slots,.nodes=bridge_nodes};

static const ksn_schema_slot companion_slots[]={
    {"head",KSN_SLOT_TEXT,47,0,0},{"line0",KSN_SLOT_TEXT,47,0,0},
    {"line1",KSN_SLOT_TEXT,47,0,0},{"line2",KSN_SLOT_TEXT,47,0,0},
    {"line3",KSN_SLOT_TEXT,47,0,0},{"foot",KSN_SLOT_TEXT,47,0,0},
    {"hint",KSN_SLOT_TEXT,47,0,0},{"variant",KSN_SLOT_U16,0,0,0},
    {"resource",KSN_SLOT_RESOURCE,0,0,0}
};
static const ksn_schema_node companion_nodes[]={
    {.kind=KSN_NODE_RECT,.bounds=RECT(8,27,98,103),.color=COLOR(0x203446ffu)},
    {.kind=KSN_NODE_IMAGE,.bounds=RECT(20,32,84,96),.resource={.slot=8},
     .variant={.slot=7},.frame={.slot=LIT,.literal.number=0},
     .source_width=64,.source_height=64},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(10,8,235,18),.color=COLOR(0x70e0d1ffu),
     .text={.slot=0},.font=KSN_CAPTION},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(106,32,236,44),.color=COLOR(0xfbe8b8ffu),
     .text={.slot=1},.font=KSN_CAPTION},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(106,50,236,62),.color=COLOR(0xbdcedbffu),
     .text={.slot=2},.font=KSN_CAPTION},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(106,68,236,80),.color=COLOR(0xbdcedbffu),
     .text={.slot=3},.font=KSN_CAPTION},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(106,86,236,98),.color=COLOR(0xbdcedbffu),
     .text={.slot=4},.font=KSN_CAPTION},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(9,113,237,123),.color=COLOR(0xfbe8b8ffu),
     .text={.slot=5},.font=KSN_CAPTION},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(9,125,237,133),.color=COLOR(0x89a4bffu),
     .text={.slot=6},.font=KSN_CAPTION}
};
static const ksn_schema companion={.version=1,.slot_count=9,.node_count=9,
    .background=0x091323ffu,.slots=companion_slots,.nodes=companion_nodes};

static const ksn_schema_slot pet_slots[]={
    {"title",KSN_SLOT_TEXT,47,0,0},{"index",KSN_SLOT_TEXT,5,0,0},
    {"species",KSN_SLOT_TEXT,12,0,0},{"status",KSN_SLOT_TEXT,16,0,0},
    {"food",KSN_SLOT_TEXT,16,0,0},{"joy",KSN_SLOT_TEXT,16,0,0},
    {"energy",KSN_SLOT_TEXT,16,0,0},{"foot",KSN_SLOT_TEXT,47,0,0},
    {"hint",KSN_SLOT_TEXT,47,0,0},{"note",KSN_SLOT_TEXT,47,0,0},
    {"resource",KSN_SLOT_RESOURCE,0,0,0},{"variant",KSN_SLOT_U16,0,0,11},
    {"frame",KSN_SLOT_U16,0,0,5},{"petY",KSN_SLOT_U16,0,0,71},
    {"bar0",KSN_SLOT_U16,0,1,106},{"bar1",KSN_SLOT_U16,0,1,106},
    {"bar2",KSN_SLOT_U16,0,1,106},{"ready",KSN_SLOT_BOOL,0,0,0},
    {"bubble",KSN_SLOT_BOOL,0,0,0},{"background",KSN_SLOT_COLOR,0,0,0},
    {"reveal",KSN_SLOT_U16,0,0,47}
};
static const ksn_schema_node pet_nodes[]={
    {.kind=KSN_NODE_TEXT,.bounds=RECT(9,7,184,21),.color=COLOR(0xf5eedcffu),
     .text={.slot=0},.font=KSN_CAPTION},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(190,7,238,21),.color=COLOR(0x67dfc7ffu),
     .text={.slot=1},.font=KSN_CAPTION},
    {.kind=KSN_NODE_RECT,.bounds=RECT(7,25,112,109),.color=COLOR(0x1c3043ffu)},
    {.kind=KSN_NODE_RECT,.bounds=RECT(15,94,104,96),.color=COLOR(0x476275ffu)},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(15,99,111,108),.color=COLOR(0xb8c7d6ffu),
     .text={.slot=2},.font=KSN_CAPTION},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(122,29,238,43),.color=COLOR(0x67dfc7ffu),
     .text={.slot=3},.font=KSN_CAPTION},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(122,45,232,57),.color=COLOR(0xc9d5dfffu),
     .text={.slot=4},.font=KSN_CAPTION},
    {.kind=KSN_NODE_RECT,.bounds=RECT(122,56,122,59),.color=COLOR(0xf5bb69ffu),
     .rect_add_mask=KSN_SCHEMA_ADD_X1,.rect_add_slot={0,0,14,0}},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(122,63,232,75),.color=COLOR(0xc9d5dfffu),
     .text={.slot=5},.font=KSN_CAPTION},
    {.kind=KSN_NODE_RECT,.bounds=RECT(122,74,122,77),.color=COLOR(0xf08bbcffu),
     .rect_add_mask=KSN_SCHEMA_ADD_X1,.rect_add_slot={0,0,15,0}},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(122,81,232,93),.color=COLOR(0xc9d5dfffu),
     .text={.slot=6},.font=KSN_CAPTION},
    {.kind=KSN_NODE_RECT,.bounds=RECT(122,92,122,95),.color=COLOR(0x67dfc7ffu),
     .rect_add_mask=KSN_SCHEMA_ADD_X1,.rect_add_slot={0,0,16,0}},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(8,114,238,124),.color=COLOR(0xf5bb69ffu),
     .text={.slot=7},.font=KSN_CAPTION},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(8,126,238,135),.color=COLOR(0x91a6baffu),
     .text={.slot=8},.font=KSN_CAPTION},
    {.kind=KSN_NODE_IMAGE,.bounds=RECT(27,0,91,64),.resource={.slot=10},
     .variant={.slot=11},.frame={.slot=12},.source_width=64,.source_height=64,
     .flags=KSN_SCHEMA_HAS_VISIBLE,.visible={.slot=17},
     .rect_add_mask=KSN_SCHEMA_ADD_Y0|KSN_SCHEMA_ADD_Y1,
     .rect_add_slot={0,13,0,13}},
    {.kind=KSN_NODE_RECT,.bounds=RECT(96,24,236,45),.color=COLOR(0x080c21ffu),
     .flags=KSN_SCHEMA_HAS_VISIBLE,.visible={.slot=18}},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(100,31,232,39),.color=COLOR(0x080c21ffu),
     .text={.slot=9},.font=KSN_CAPTION,
     .flags=KSN_SCHEMA_HAS_VISIBLE|KSN_SCHEMA_HAS_REVEAL,
     .visible={.slot=18},.reveal={.slot=20}}
};
static const ksn_schema pet={.version=1,.slot_count=21,.node_count=17,
    .background=0x0b1727ffu,.dynamic_background=true,.background_slot=19,
    .slots=pet_slots,.nodes=pet_nodes};

static const ksn_schema_slot clock_slots[]={
    {"face",KSN_SLOT_TEXT,47,0,0},{"tag",KSN_SLOT_TEXT,47,0,0}
};
static const ksn_schema_node clock_nodes[]={
    {.kind=KSN_NODE_RECT,.bounds=RECT(0,0,96,22),.color=COLOR(0x060c1affu)},
    {.kind=KSN_NODE_RECT,.bounds=RECT(0,0,96,1),.color=COLOR(0x3edcd0ffu)},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(4,4,48,16),.color=COLOR(0xe2f0ffffu),
     .text={.slot=0},.font=KSN_CAPTION},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(50,12,94,24),.color=COLOR(0x63d6ddffu),
     .text={.slot=1},.font=KSN_CAPTION}
};
static const ksn_schema clock_view={.version=1,.slot_count=2,.node_count=4,
    .background=0x000000ffu,.slots=clock_slots,.nodes=clock_nodes};
static const ksn_source_binding clock_bindings[]={{0,0},{1,1}};
static const pocket_app_view_source clock_sources[]={
    {.source_bytes=sizeof(pocket_clock_source_state),.source_open=pocket_clock_source_open,
     .source_registered=pocket_clock_source_registered,.source_bindings=clock_bindings,
     .source_binding_count=2}
};
static const pocket_app_view_asset hello_asset={.schema=&hello};
static const pocket_app_view_asset imucal_asset={.schema=&imucal};
static const pocket_app_view_asset bridge_asset={.schema=&bridge};
static const pocket_app_view_asset companion_asset={.schema=&companion};
static const pocket_app_view_asset pet_asset={.schema=&pet};
static const pocket_app_view_asset clock_asset={.schema=&clock_view,
    .sources=clock_sources,.source_count=1};
#ifdef KSN_TEST_DUAL_SOURCE
/* Host-only mount fixture: two independent native producers with disjoint
 * slots. No test state or app identity enters the Kasane core. */
static const ksn_schema_slot dual_slots[]={
    {"left",KSN_SLOT_TEXT,7,0,0},{"right",KSN_SLOT_TEXT,7,0,0},
    {"alt",KSN_SLOT_TEXT,7,0,0},{"tint",KSN_SLOT_COLOR,0,0,0}
};
static const ksn_schema_node dual_nodes[]={
    {.kind=KSN_NODE_TEXT,.bounds=RECT(0,0,45,16),
     .color=COLOR(0xffffffffu),.text={.slot=0},.font=KSN_CAPTION},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(48,0,62,16),
     .color=COLOR(0xffffffffu),.text={.slot=1},.font=KSN_CAPTION},
    {.kind=KSN_NODE_TEXT,.bounds=RECT(64,0,95,16),
     .color=COLOR(0xffffffffu),.text={.slot=2},.font=KSN_CAPTION}
};
static const ksn_schema dual_view={.version=1,.slot_count=4,.node_count=3,
    .background=0x000000ffu,.slots=dual_slots,.nodes=dual_nodes};
typedef struct {unsigned id;uint32_t generation;ksn_schema_value field;} dual_source;
static struct {char text[8];uint64_t revision,expires_at_us;uint32_t changed;} dual_model[2]={
    {.text="A0",.revision=1,.changed=1},
    {.text="B0",.revision=1,.changed=1}
};
static bool dual_fail_second;
static bool dual_deny[2];
static unsigned dual_acquired,dual_released;
void pocket_test_dual_set(unsigned source,const char *text){
    if(source>=2||!text)return;
    size_t n=strlen(text);if(n>7)return;
    memcpy(dual_model[source].text,text,n+1u);
    dual_model[source].revision++;
    dual_model[source].changed=1;
}
void pocket_test_dual_expire(unsigned source,uint64_t at_us){
    if(source>=2)return;
    dual_model[source].expires_at_us=at_us;
    dual_model[source].revision++;
    dual_model[source].changed=0;
}
void pocket_test_dual_deny(unsigned source,bool deny){
    if(source<2)dual_deny[source]=deny;
}
void pocket_test_dual_fail_second(bool fail){dual_fail_second=fail;}
void pocket_test_dual_counts(unsigned *acquired,unsigned *released){
    if(acquired)*acquired=dual_acquired;
    if(released)*released=dual_released;
}
static ksn_result dual_acquire(void *opaque,uint64_t cursor,uint64_t now_us,
                               ksn_source_snapshot *out){
    (void)cursor;(void)now_us;
    dual_source *source=opaque;
    if(source->id==1&&dual_fail_second)return KSN_IO;
    unsigned id=source->id;
    source->field.data.text=(ksn_schema_text){dual_model[id].text,
        (uint16_t)strlen(dual_model[id].text)};
    *out=(ksn_source_snapshot){.size=sizeof(*out),.version=KSN_SOURCE_ABI_VERSION,
        .field_count=1,.generation=source->generation,
        .revision=dual_model[id].revision,.expires_at_us=dual_model[id].expires_at_us,
        .valid_fields=1,.changed_fields=dual_model[id].changed,
        .fields=&source->field};
    dual_acquired++;
    return KSN_OK;
}
static void dual_release(void *opaque,const ksn_source_snapshot *snapshot){
    (void)opaque;(void)snapshot;dual_released++;
}
static bool dual_allow(void *opaque,uint32_t consumer){
    return consumer!=0&&!dual_deny[((dual_source *)opaque)->id];
}
static ksn_result dual_open(void *storage,ksn_source_provider *out,unsigned id){
    static const ksn_slot_type types[]={KSN_SLOT_TEXT};
    dual_source *source=storage;source->id=id;
    *out=(ksn_source_provider){.size=sizeof(*out),.version=KSN_SOURCE_ABI_VERSION,
        .field_count=1,.field_types=types,.context=storage,
        .acquire=dual_acquire,.release=dual_release,.allow=dual_allow};
    return KSN_OK;
}
static ksn_result dual_open_left(void *storage,ksn_source_provider *out){
    return dual_open(storage,out,0);
}
static ksn_result dual_open_right(void *storage,ksn_source_provider *out){
    return dual_open(storage,out,1);
}
static void dual_registered(void *storage,ksn_source_handle handle){
    ((dual_source *)storage)->generation=handle.generation;
}
static const ksn_source_binding dual_left[]={{0,0}},dual_right[]={{1,0}};
static const pocket_app_view_source dual_sources[]={
    {sizeof(dual_source),dual_open_left,dual_registered,dual_left,1},
    {sizeof(dual_source),dual_open_right,dual_registered,dual_right,1}
};
static const pocket_app_view_asset dual_asset={.schema=&dual_view,
    .sources=dual_sources,.source_count=2};
#endif
const pocket_app_view_asset *pocket_app_view_lookup(const char *name){
    if(!name)return NULL;
    if(strcmp(name,"hello")==0)return &hello_asset;
    if(strcmp(name,"imucal")==0)return &imucal_asset;
    if(strcmp(name,"bridge")==0)return &bridge_asset;
    if(strcmp(name,"companion")==0)return &companion_asset;
    if(strcmp(name,"pet")==0)return &pet_asset;
    if(strcmp(name,"clock")==0)return &clock_asset;
#ifdef KSN_TEST_DUAL_SOURCE
    if(strcmp(name,"dual-test")==0)return &dual_asset;
#endif
    return NULL;
}
static const pocket_app_image_asset pet_image_asset={"pets",64,64,12,6,ksn_pet_builtin_image};
const pocket_app_image_asset *pocket_app_image_lookup(const char *name){
    return name&&strcmp(name,"pets")==0?&pet_image_asset:NULL;
}
