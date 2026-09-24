#include "core_fixture.h"
#include "ksn_core.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define EXPECT(x) do { if(!(x)){fprintf(stderr,"review line %d: %s\n",__LINE__,#x);failures++;} } while(0)
static ksn_draw rectangle(void){
    ksn_draw d={.kind=KSN_RECT,.bounds={0,0,10,10},.clip={0,0,240,135},.opacity=255};
    d.data.shape.color=0xffffffff;return d;
}
int main(void){
    KSN_TEST_CORE(core,);ksn_core_init(&core);
    ksn_client app=ksn_core_client(&core,KSN_APP),system=ksn_core_client(&core,KSN_SYSTEM);
    ksn_tx tx,old_tx;ksn_ref ref,old_ref;ksn_draw d=rectangle();
    EXPECT(system.ops->begin(system.ctx,KSN_REPLACE,&tx)==KSN_OK);
    EXPECT(app.ops->add(app.ctx,tx,&d,&ref)==KSN_STALE);
    app.ops->abort(app.ctx,tx);
    EXPECT(system.ops->add(system.ctx,tx,&d,&ref)==KSN_OK);
    system.ops->abort(system.ctx,tx);
    EXPECT(app.ops->begin(app.ctx,KSN_REPLACE,&old_tx)==KSN_OK);
    EXPECT(app.ops->add(app.ctx,old_tx,&d,&old_ref)==KSN_OK);
    app.ops->abort(app.ctx,old_tx);
    ksn_core_init(&core);app=ksn_core_client(&core,KSN_APP);
    EXPECT(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    EXPECT(app.ops->add(app.ctx,tx,&d,&ref)==KSN_OK);
    EXPECT(tx.value!=old_tx.value&&ref.value!=old_ref.value);
    app.ops->abort(app.ctx,tx);
    EXPECT(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
    EXPECT(app.ops->add(app.ctx,tx,&d,&ref)==KSN_INVALID);
    app.ops->abort(app.ctx,tx);

    /* SYSTEM works before APP, and foreign end cannot seal its submission. */
    EXPECT(system.ops->begin(system.ctx,KSN_REPLACE,&tx)==KSN_OK);
    EXPECT(system.ops->add(system.ctx,tx,&d,&ref)==KSN_OK);
    EXPECT(app.ops->end(app.ctx,tx)==KSN_STALE);
    EXPECT(system.ops->end(system.ctx,tx)==KSN_OK);
    ksn_frame frame;ksn_frame_command command;
    EXPECT(ksn_core_frame(&core,&frame)==KSN_OK&&frame.full_redraw);
    EXPECT(frame.next[KSN_SYSTEM].commands==1&&frame.next_background==0x000000ff);
    EXPECT(ksn_core_read(&core,frame.ticket,false,KSN_SYSTEM,0,&command)==KSN_OK);
    EXPECT(command.visible&&command.draw.data.shape.color==0xffffffff);
    EXPECT(ksn_core_presented(&core,frame.ticket)==KSN_OK);
    ksn_tx stale=frame.ticket;
    EXPECT(ksn_core_read(&core,stale,false,KSN_SYSTEM,0,&command)==KSN_STALE);

    /* Text is copied in both directions. Failure survives a discarded frame. */
    EXPECT(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    EXPECT(app.ops->background(app.ctx,tx,0x112233ff)==KSN_OK);
    char input[]="abc";
    d.kind=KSN_TEXT;
    d.data.text.utf8=input;d.data.text.bytes=3;d.data.text.capacity=8;
    d.data.text.font=KSN_BODY;d.data.text.color=0xffffffff;
    EXPECT(app.ops->add(app.ctx,tx,&d,&ref)==KSN_OK);
    input[0]='z';
    EXPECT(app.ops->end(app.ctx,tx)==KSN_OK);
    EXPECT(ksn_core_frame(&core,&frame)==KSN_OK&&!frame.full_redraw);
    EXPECT(ksn_core_presented(&core,stale)==KSN_STALE);
    EXPECT(ksn_core_discard(&core,stale)==KSN_STALE);
    EXPECT(ksn_core_read(&core,frame.ticket,false,KSN_APP,0,&command)==KSN_OK);
    EXPECT(command.draw.data.text.utf8==command.text&&memcmp(command.text,"abc",3)==0);
    EXPECT(ksn_core_read_borrowed(&core,frame.ticket,false,KSN_APP,0,&command)==KSN_OK);
    EXPECT(command.draw.data.text.utf8!=command.text&&
           memcmp(command.draw.data.text.utf8,"abc",3)==0);
    EXPECT(ksn_core_presented(&core,frame.ticket)==KSN_OK);
    EXPECT(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
    ksn_change change={.property=KSN_SET_TEXT,.value.text={"xy",2}};
    EXPECT(app.ops->change(app.ctx,tx,ref,&change)==KSN_OK);
    EXPECT(app.ops->end(app.ctx,tx)==KSN_OK);
    EXPECT(ksn_core_frame(&core,&frame)==KSN_OK);
    EXPECT(ksn_core_read(&core,frame.ticket,true,KSN_APP,0,&command)==KSN_OK);
    EXPECT(command.draw.data.text.bytes==3&&memcmp(command.text,"abc",3)==0);
    EXPECT(ksn_core_read(&core,frame.ticket,false,KSN_APP,0,&command)==KSN_OK);
    EXPECT(command.draw.data.text.bytes==2&&memcmp(command.text,"xy",2)==0);
    EXPECT(ksn_core_read_borrowed(&core,frame.ticket,true,KSN_APP,0,&command)==KSN_OK);
    EXPECT(command.draw.data.text.bytes==3&&memcmp(command.draw.data.text.utf8,"abc",3)==0);
    EXPECT(ksn_core_read_borrowed(&core,frame.ticket,false,KSN_APP,0,&command)==KSN_OK);
    EXPECT(command.draw.data.text.bytes==2&&memcmp(command.draw.data.text.utf8,"xy",2)==0);
    EXPECT(ksn_core_failed(&core,frame.ticket)==KSN_OK);
    EXPECT(ksn_core_read_borrowed(&core,frame.ticket,false,KSN_APP,0,&command)==KSN_OK);
    EXPECT(memcmp(command.draw.data.text.utf8,"xy",2)==0);
    EXPECT(ksn_core_discard(&core,frame.ticket)==KSN_OK);
    EXPECT(ksn_core_read_borrowed(&core,frame.ticket,false,KSN_APP,0,&command)==KSN_STALE);
    EXPECT(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
    EXPECT(app.ops->end(app.ctx,tx)==KSN_OK);
    EXPECT(ksn_core_frame(&core,&frame)==KSN_OK&&frame.full_redraw);
    EXPECT(ksn_core_presented(&core,frame.ticket)==KSN_OK);

    /* Reject excessive lengths before inspecting the caller's bytes. */
    EXPECT(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
    change.value.text.utf8="x";change.value.text.bytes=UINT16_MAX;
    EXPECT(app.ops->change(app.ctx,tx,ref,&change)==KSN_LIMIT);
    app.ops->abort(app.ctx,tx);
    EXPECT(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
    change.value.text.utf8=NULL;change.value.text.bytes=0;
    EXPECT(app.ops->change(app.ctx,tx,ref,&change)==KSN_OK);
    EXPECT(app.ops->end(app.ctx,tx)==KSN_OK);
    EXPECT(ksn_core_frame(&core,&frame)==KSN_OK);
    ksn_core_init(&core);
    EXPECT(ksn_core_presented(&core,frame.ticket)==KSN_STALE);
    EXPECT(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    EXPECT(app.ops->background(app.ctx,tx,0x000000ff)==KSN_OK);
    d=rectangle();EXPECT(app.ops->add(app.ctx,tx,&d,&old_ref)==KSN_OK);
    EXPECT(app.ops->change(app.ctx,tx,ref,&change)==KSN_STALE);
    app.ops->abort(app.ctx,tx);
    if(failures)return 1;
    puts("review regressions: PASS");return 0;
}
