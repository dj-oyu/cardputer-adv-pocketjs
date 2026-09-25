#include "core_fixture.h"
#include "ksn_core.h"
#include "use_cases.h"
#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { if(!(condition)){ \
    fprintf(stderr,"core failure at line %d: %s\n",__LINE__,#condition);return 1; \
} } while(0)

static ksn_result begin_app(ksn_client app,ksn_update_mode mode,ksn_tx *tx){
    ksn_result result=app.ops->begin(app.ctx,mode,tx);
    if(result==KSN_OK&&mode==KSN_REPLACE)result=app.ops->background(app.ctx,*tx,0x0b1727ff);
    return result;
}
static ksn_result presented(ksn_core *core){
    ksn_frame frame;ksn_result result=ksn_core_frame(core,&frame);
    return result==KSN_OK?ksn_core_presented(core,frame.ticket):result;
}
static ksn_result discard(ksn_core *core){
    ksn_frame frame;ksn_result result=ksn_core_frame(core,&frame);
    return result==KSN_OK?ksn_core_discard(core,frame.ticket):result;
}
static ksn_result add_rect(ksn_client client,ksn_tx tx,ksn_ref *ref){
    ksn_draw draw={.kind=KSN_RECT,.bounds={1,2,11,12},.clip={0,0,240,135},.opacity=255};
    draw.data.shape.color=0x112233ff;
    return client.ops->add(client.ctx,tx,&draw,ref);
}
static ksn_result add_text(ksn_client client,ksn_tx tx,const char *text,uint16_t bytes,
                          uint16_t capacity,ksn_ref *ref){
    ksn_draw draw={.kind=KSN_TEXT,.bounds={1,2,100,16},.clip={0,0,240,135},.opacity=255};
    draw.data.text=(typeof(draw.data.text)){text,bytes,capacity,KSN_BODY,0xf5eedcff};
    return client.ops->add(client.ctx,tx,&draw,ref);
}
static const ksn_command_storage *selected_command(const ksn_bank *bank,unsigned index){
    unsigned owner=(bank->command_owner[index/32u]>>(index%32u))&1u;
    return (owner==bank->physical_index?bank->commands:bank->command_peer)+index;
}

static ksn_result test_image_span(void *ctx,uint16_t variant,uint16_t frame,uint16_t y,
                                 uint16_t x,uint16_t count,uint16_t *rgb,uint8_t *alpha){
    (void)ctx;(void)variant;(void)frame;(void)y;(void)x;
    for(unsigned i=0;i<count;i++){rgb[i]=0xffff;alpha[i]=255;}
    return KSN_OK;
}
int main(void){
    KSN_TEST_CORE(core,);
    CHECK(ksn_core_bind(&core,&core_commands[0],&core_commands[1],
                        &core_text[0],&core_text[1])==KSN_OK);
    ksn_client app=ksn_core_client(&core,KSN_APP),system=ksn_core_client(&core,KSN_SYSTEM);
    CHECK(app.ops&&system.ops&&ksn_core_client(&core,(ksn_layer)9).ops==NULL);
    ksn_limits limits=app.ops->limits(app.ctx);
    CHECK(limits.app.commands==80&&limits.app.text_bytes==896);
    CHECK(limits.system.commands==16&&limits.system.text_bytes==128);
    CHECK(limits.native_bytes==KSN_CORE_RESERVED_BYTES+4*sizeof(uint32_t));
    CHECK(limits.app.tracks==6&&limits.system.tracks==2);
    printf("core native budget: %lu bytes (including shared IDs)\n",(unsigned long)limits.native_bytes);

    CHECK(ksn_core_opaque_system_bands(&core)==0);
    ksn_tx occlusion_tx;ksn_ref occlusion_ref;
    ksn_draw occlusion={.kind=KSN_RECT,.bounds={0,0,240,48},
                        .clip={0,0,240,48},.opacity=255,
                        .data.shape={.color=0x080c20ff}};
    CHECK(system.ops->begin(system.ctx,KSN_REPLACE,&occlusion_tx)==KSN_OK);
    CHECK(system.ops->add(system.ctx,occlusion_tx,&occlusion,&occlusion_ref)==KSN_OK);
    CHECK(system.ops->end(system.ctx,occlusion_tx)==KSN_OK);
    CHECK(ksn_core_opaque_system_bands(&core)==0); /* pending is unsafe */
    CHECK(presented(&core)==KSN_OK);
    CHECK(ksn_core_opaque_system_bands(&core)==0x3fu);
    CHECK(system.ops->begin(system.ctx,KSN_REPLACE,&occlusion_tx)==KSN_OK);
    CHECK(system.ops->end(system.ctx,occlusion_tx)==KSN_OK);
    CHECK(ksn_core_opaque_system_bands(&core)==0);
    CHECK(presented(&core)==KSN_OK);
    CHECK(ksn_core_opaque_system_bands(&core)==0);

    pet_view view;
    ksn_image_port image={NULL,64,64,16,16,test_image_span};ksn_resource resource;
    CHECK(ksn_core_register_image(&core,KSN_APP,&image,&resource)==KSN_OK);
    CHECK(pet_view_build(app,resource,&view)==KSN_OK);
    CHECK(ksn_core_has_submission(&core));
    CHECK(ksn_core_active_usage(&core,KSN_APP).commands==0);
    CHECK(ksn_core_submission_usage(&core,KSN_APP).commands==3);
    ksn_tx blocked;
    CHECK(app.ops->begin(app.ctx,KSN_PATCH,&blocked)==KSN_BUSY);
    CHECK(presented(&core)==KSN_OK&&!ksn_core_has_submission(&core));
    CHECK(ksn_core_active_usage(&core,KSN_APP).commands==3);

    CHECK(pet_view_update(app,&view,100)==KSN_OK);
    CHECK(presented(&core)==KSN_OK);
    CHECK(notice_view_show(system,"ALARM",5)==KSN_OK);
    CHECK(ksn_core_submission_usage(&core,KSN_APP).commands==3);
    CHECK(ksn_core_submission_usage(&core,KSN_SYSTEM).commands==1);
    CHECK(presented(&core)==KSN_OK);

    /* A SYSTEM replace leaves APP references and generation intact. */
    CHECK(pet_view_update(app,&view,50)==KSN_OK);
    CHECK(presented(&core)==KSN_OK);

    /* Discard keeps the displayed baseline and old references usable. */
    ksn_tx tx;ksn_ref discarded;
    CHECK(begin_app(app,KSN_REPLACE,&tx)==KSN_OK);
    CHECK(add_rect(app,tx,&discarded)==KSN_OK);
    CHECK(app.ops->end(app.ctx,tx)==KSN_OK);
    CHECK(discard(&core)==KSN_OK);
    CHECK(ksn_core_active_usage(&core,KSN_APP).commands==3);
    CHECK(pet_view_update(app,&view,40)==KSN_OK);
    CHECK(presented(&core)==KSN_OK);

    /* Invalid rebinding must preserve both banks and the displayed refs. */
    ksn_core saved=core;
    CHECK(ksn_core_bind(&core,NULL,&core_commands[1],&core_text[0],&core_text[1])==KSN_INVALID);
    CHECK(ksn_core_bind(&core,&core_commands[0],&core_commands[0],&core_text[0],&core_text[1])==KSN_INVALID);
    CHECK(ksn_core_bind(&core,&core_commands[0],&core_commands[1],&core_text[0],&core_text[0])==KSN_INVALID);
    CHECK(memcmp(&saved,&core,sizeof(core))==0);
    CHECK(pet_view_update(app,&view,35)==KSN_OK);
    CHECK(presented(&core)==KSN_OK);

    /* APP replace invalidates prior APP refs after presentation. */
    CHECK(begin_app(app,KSN_REPLACE,&tx)==KSN_OK);
    ksn_ref replacement;CHECK(add_rect(app,tx,&replacement)==KSN_OK);
    CHECK(app.ops->end(app.ctx,tx)==KSN_OK&&presented(&core)==KSN_OK);
    CHECK(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
    ksn_change change={.property=KSN_SET_VISIBLE,.value.visible=false};
    CHECK(app.ops->change(app.ctx,tx,view.pet,&change)==KSN_STALE);
    CHECK(app.ops->end(app.ctx,tx)==KSN_STALE);
    app.ops->abort(app.ctx,tx);

    /* Invalid text poisons only the open transaction and publishes nothing. */
    CHECK(begin_app(app,KSN_REPLACE,&tx)==KSN_OK);
    ksn_ref text;
    CHECK(add_text(app,tx,"A\n",2,8,&text)==KSN_INVALID);
    CHECK(add_rect(app,tx,&text)==KSN_INVALID);
    CHECK(app.ops->end(app.ctx,tx)==KSN_INVALID);
    app.ops->abort(app.ctx,tx);
    CHECK(ksn_core_active_usage(&core,KSN_APP).commands==1);

    ksn_core_init(&core);app=ksn_core_client(&core,KSN_APP);
    CHECK(begin_app(app,KSN_REPLACE,&tx)==KSN_OK);
    for(unsigned i=0;i<7;i++)CHECK(add_text(app,tx,"",0,128,&text)==KSN_OK);
    CHECK(add_text(app,tx,"",0,1,&text)==KSN_LIMIT);
    CHECK(app.ops->end(app.ctx,tx)==KSN_LIMIT);
    app.ops->abort(app.ctx,tx);
    CHECK(!ksn_core_has_submission(&core));

    ksn_core_init(&core);app=ksn_core_client(&core,KSN_APP);
    CHECK(begin_app(app,KSN_REPLACE,&tx)==KSN_OK);
    CHECK(add_text(app,tx,"12345",5,4,&text)==KSN_LIMIT);
    app.ops->abort(app.ctx,tx);

    ksn_core_init(&core);app=ksn_core_client(&core,KSN_APP);
    CHECK(begin_app(app,KSN_REPLACE,&tx)==KSN_OK);
    for(unsigned i=0;i<KSN_APP_COMMANDS;i++)CHECK(add_rect(app,tx,&replacement)==KSN_OK);
    CHECK(add_rect(app,tx,&replacement)==KSN_LIMIT);
    app.ops->abort(app.ctx,tx);

    ksn_core_init(&core);
    for(unsigned i=0;i<2;i++) {
        CHECK(core.state.banks[i].commands==core_commands[i].commands);
        CHECK(core.state.banks[i].text==core_text[i].bytes);
        const unsigned char *bytes=(const unsigned char *)&core_commands[i];
        for(size_t n=0;n<sizeof(core_commands[i]);n++)CHECK(bytes[n]==0);
        for(size_t n=0;n<sizeof(core_text[i].bytes);n++)CHECK(core_text[i].bytes[n]==0);
    }

    /* PATCH borrows sealed commands and both text layers. A writer splits
     * only its command slot and text layer. Poison only unused physical slots,
     * never a region that the displayed bank may borrow. */
    app=ksn_core_client(&core,KSN_APP);system=ksn_core_client(&core,KSN_SYSTEM);
    CHECK(begin_app(app,KSN_REPLACE,&tx)==KSN_OK);
    ksn_ref app_text,app_rect,system_text;
    CHECK(add_text(app,tx,"abc",3,8,&app_text)==KSN_OK);
    CHECK(add_rect(app,tx,&app_rect)==KSN_OK);
    CHECK(app.ops->end(app.ctx,tx)==KSN_OK&&presented(&core)==KSN_OK);
    CHECK(system.ops->begin(system.ctx,KSN_REPLACE,&tx)==KSN_OK);
    CHECK(add_text(system,tx,"SYS",3,8,&system_text)==KSN_OK);
    CHECK(system.ops->end(system.ctx,tx)==KSN_OK&&presented(&core)==KSN_OK);
    ksn_bank *active=&core.state.banks[core.state.active];
    ksn_bank *spare=&core.state.banks[core.state.active^1u];
    uint8_t *app_before=active->text_layer[KSN_APP];
    uint8_t *system_before=active->text_layer[KSN_SYSTEM];
    CHECK(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
    change=(ksn_change){.property=KSN_SET_COLOR,.value.color=0x123456ff};
    CHECK(app.ops->change(app.ctx,tx,app_text,&change)==KSN_OK);
    CHECK(spare->text_layer[KSN_APP]==app_before&&
          spare->text_layer[KSN_SYSTEM]==system_before);
    CHECK(app.ops->end(app.ctx,tx)==KSN_OK&&presented(&core)==KSN_OK);
    active=&core.state.banks[core.state.active];
    spare=&core.state.banks[core.state.active^1u];
    CHECK(active->text_layer[KSN_APP]==app_before&&
          active->text_layer[KSN_SYSTEM]==system_before);
    memset(spare->commands+2,0xa5,(KSN_APP_COMMANDS-2)*sizeof(ksn_command_storage));
    memset(spare->commands+KSN_APP_COMMANDS+1,0xa5,
           (KSN_SYSTEM_COMMANDS-1)*sizeof(ksn_command_storage));
    uint8_t *app_target=active->text_layer[KSN_APP]==core_text[0].bytes?
        core_text[1].bytes:core_text[0].bytes;
    memset(app_target,0xa5,KSN_APP_TEXT_BYTES);
    CHECK(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
    CHECK(selected_command(spare,0)==selected_command(active,0));
    CHECK(selected_command(spare,1)==selected_command(active,1));
    CHECK(selected_command(spare,KSN_APP_COMMANDS)==
          selected_command(active,KSN_APP_COMMANDS));
    CHECK(spare->text_layer[KSN_APP]==active->text_layer[KSN_APP]&&
          spare->text_layer[KSN_SYSTEM]==active->text_layer[KSN_SYSTEM]);
    CHECK(app_target[0]==0xa5);
    CHECK(((unsigned char *)(spare->commands+2))[0]==0xa5);
    CHECK(((unsigned char *)(spare->commands+KSN_APP_COMMANDS+1))[0]==0xa5);
    CHECK(app_target[8]==0xa5);
    change=(ksn_change){.property=KSN_SET_TEXT,.value.text={"XY",2}};
    CHECK(app.ops->change(app.ctx,tx,app_text,&change)==KSN_OK);
    CHECK(selected_command(spare,0)!=selected_command(active,0));
    CHECK(selected_command(spare,1)==selected_command(active,1));
    CHECK(spare->text_layer[KSN_APP]==app_target&&app_target[0]=='X'&&
          memcmp(active->text_layer[KSN_SYSTEM],"SYS",3)==0);
    CHECK(app.ops->end(app.ctx,tx)==KSN_OK&&presented(&core)==KSN_OK);
    ksn_frame_command read={0};
    CHECK(ksn_core_read_active_ref(&core,KSN_APP,app_text,&read)==KSN_OK);
    CHECK(read.draw.data.text.bytes==2&&memcmp(read.draw.data.text.utf8,"XY",2)==0);
    CHECK(ksn_core_read_active_ref(&core,KSN_SYSTEM,system_text,&read)==KSN_OK);
    CHECK(read.draw.data.text.bytes==3&&memcmp(read.draw.data.text.utf8,"SYS",3)==0);
    /* Alternating writers must preserve the other layer's borrowed region
     * through a pending ticket and after the physical bank flips again. */
    CHECK(system.ops->begin(system.ctx,KSN_PATCH,&tx)==KSN_OK);
    active=&core.state.banks[core.state.active];
    spare=&core.state.banks[core.state.building_bank];
    CHECK(spare->text_layer[KSN_APP]==active->text_layer[KSN_APP]);
    CHECK(spare->text_layer[KSN_SYSTEM]==active->text_layer[KSN_SYSTEM]);
    change=(ksn_change){.property=KSN_SET_TEXT,.value.text={"NEW",3}};
    CHECK(system.ops->change(system.ctx,tx,system_text,&change)==KSN_OK);
    CHECK(spare->text_layer[KSN_SYSTEM]!=active->text_layer[KSN_SYSTEM]);
    CHECK(ksn_core_read_active_ref(&core,KSN_SYSTEM,system_text,&read)==KSN_OK);
    CHECK(memcmp(read.draw.data.text.utf8,"SYS",3)==0);
    CHECK(system.ops->end(system.ctx,tx)==KSN_OK&&presented(&core)==KSN_OK);
    CHECK(ksn_core_read_active_ref(&core,KSN_APP,app_text,&read)==KSN_OK);
    CHECK(memcmp(read.draw.data.text.utf8,"XY",2)==0);
    CHECK(ksn_core_read_active_ref(&core,KSN_SYSTEM,system_text,&read)==KSN_OK);
    CHECK(memcmp(read.draw.data.text.utf8,"NEW",3)==0);
    CHECK(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
    change=(ksn_change){.property=KSN_SET_TEXT,.value.text={"OK",2}};
    CHECK(app.ops->change(app.ctx,tx,app_text,&change)==KSN_OK);
    CHECK(app.ops->end(app.ctx,tx)==KSN_OK&&presented(&core)==KSN_OK);
    CHECK(ksn_core_read_active_ref(&core,KSN_SYSTEM,system_text,&read)==KSN_OK);
    CHECK(memcmp(read.draw.data.text.utf8,"NEW",3)==0);
    CHECK(ksn_core_read_active_ref(&core,KSN_APP,app_text,&read)==KSN_OK);
    CHECK(memcmp(read.draw.data.text.utf8,"OK",2)==0);
    CHECK(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
    active=&core.state.banks[core.state.active];
    spare=&core.state.banks[core.state.building_bank];
    change=(ksn_change){.property=KSN_SET_TEXT,.value.text={"123456789",9}};
    CHECK(app.ops->change(app.ctx,tx,app_text,&change)==KSN_LIMIT);
    CHECK(spare->text_layer[KSN_APP]==active->text_layer[KSN_APP]);
    app.ops->abort(app.ctx,tx);
    CHECK(ksn_core_read_active_ref(&core,KSN_APP,app_text,&read)==KSN_OK);
    CHECK(memcmp(read.draw.data.text.utf8,"OK",2)==0);
    CHECK(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
    change=(ksn_change){.property=KSN_SET_REVEAL,.value.reveal=1};
    CHECK(app.ops->change(app.ctx,tx,app_text,&change)==KSN_OK);
    CHECK(app.ops->end(app.ctx,tx)==KSN_OK&&presented(&core)==KSN_OK);
    CHECK(ksn_core_read_active_ref(&core,KSN_APP,app_text,&read)==KSN_OK);
    CHECK(read.reveal==1&&read.draw.data.text.bytes==2&&memcmp(read.draw.data.text.utf8,"OK",2)==0);
    CHECK(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
    change=(ksn_change){.property=KSN_SET_TEXT,.value.text={"END",3}};
    CHECK(app.ops->change(app.ctx,tx,app_text,&change)==KSN_OK);
    change=(ksn_change){.property=KSN_SET_REVEAL,.value.reveal=2};
    CHECK(app.ops->change(app.ctx,tx,app_text,&change)==KSN_OK);
    app.ops->abort(app.ctx,tx);
    CHECK(ksn_core_read_active_ref(&core,KSN_APP,app_text,&read)==KSN_OK);
    CHECK(read.reveal==1&&memcmp(read.draw.data.text.utf8,"OK",2)==0);
    CHECK(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
    CHECK(app.ops->end(app.ctx,tx)==KSN_OK&&presented(&core)==KSN_OK);
    CHECK(ksn_core_read_active_ref(&core,KSN_APP,app_text,&read)==KSN_OK);
    CHECK(read.draw.data.text.bytes==2&&memcmp(read.draw.data.text.utf8,"OK",2)==0);
    CHECK(ksn_core_read_active_ref(&core,KSN_SYSTEM,system_text,&read)==KSN_OK);
    CHECK(read.draw.data.text.bytes==3&&memcmp(read.draw.data.text.utf8,"NEW",3)==0);
    /* Group members need not occupy one physical command array after
     * per-slot borrowing. Exercise both REPLACE and PATCH on mixed owners. */
    active=&core.state.banks[core.state.active];
    if(((active->command_owner[0]^(active->command_owner[0]>>1))&1u)==0){
        CHECK(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
        change=(ksn_change){.property=KSN_SET_VISIBLE,.value.visible=false};
        CHECK(app.ops->change(app.ctx,tx,app_rect,&change)==KSN_OK);
        CHECK(app.ops->end(app.ctx,tx)==KSN_OK&&presented(&core)==KSN_OK);
    }
    active=&core.state.banks[core.state.active];
    CHECK(((active->command_owner[0]^(active->command_owner[0]>>1))&1u)!=0);
    CHECK(begin_app(app,KSN_REPLACE,&tx)==KSN_OK);
    ksn_ref group_first,group_second;
    CHECK(add_rect(app,tx,&group_first)==KSN_OK);
    CHECK(add_rect(app,tx,&group_second)==KSN_OK);
    CHECK(ksn_core_group(&core,KSN_APP,tx,group_first,2,128)==KSN_OK);
    CHECK(app.ops->end(app.ctx,tx)==KSN_OK&&presented(&core)==KSN_OK);
    CHECK(ksn_core_read_active_ref(&core,KSN_APP,group_second,&read)==KSN_OK);
    CHECK(read.group_end&&read.group_opacity==128);
    CHECK(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
    CHECK(ksn_core_group(&core,KSN_APP,tx,group_first,2,160)==KSN_OK);
    CHECK(app.ops->end(app.ctx,tx)==KSN_OK&&presented(&core)==KSN_OK);
    CHECK(ksn_core_read_active_ref(&core,KSN_APP,group_first,&read)==KSN_OK);
    CHECK(read.group_begin&&read.group_opacity==160);
    CHECK(ksn_core_read_active_ref(&core,KSN_APP,group_second,&read)==KSN_OK);
    CHECK(read.group_end&&read.group_opacity==160);
    /* A caller may keep a read-only active text pointer until presentation.
     * REPLACE must not reuse its physical storage while the candidate builds,
     * even when the new scene assigns the same logical text offset. */
    CHECK(begin_app(app,KSN_REPLACE,&tx)==KSN_OK);
    ksn_ref old_borrow,new_borrow;
    CHECK(add_text(app,tx,"OLD",3,8,&old_borrow)==KSN_OK);
    CHECK(app.ops->end(app.ctx,tx)==KSN_OK&&presented(&core)==KSN_OK);
    CHECK(ksn_core_read_active_ref(&core,KSN_APP,old_borrow,&read)==KSN_OK);
    const char *held_text=read.draw.data.text.utf8;
    CHECK(memcmp(held_text,"OLD",3)==0);
    CHECK(begin_app(app,KSN_REPLACE,&tx)==KSN_OK);
    CHECK(memcmp(held_text,"OLD",3)==0);
    CHECK(add_text(app,tx,"NEW",3,8,&new_borrow)==KSN_OK);
    CHECK(memcmp(held_text,"OLD",3)==0);
    CHECK(app.ops->end(app.ctx,tx)==KSN_OK);
    CHECK(memcmp(held_text,"OLD",3)==0);
    CHECK(discard(&core)==KSN_OK);
    CHECK(ksn_core_read_active_ref(&core,KSN_APP,old_borrow,&read)==KSN_OK);
    CHECK(memcmp(read.draw.data.text.utf8,"OLD",3)==0);
    puts("fixed core: PASS (borrowed banks, failed bind, reset)");return 0;
}
