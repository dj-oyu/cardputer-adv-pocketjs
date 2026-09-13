#include "ds_core.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define EXPECT(x) do { if(!(x)){fprintf(stderr,"review line %d: %s\n",__LINE__,#x);failures++;} } while(0)
static ds_draw rectangle(void){
    ds_draw d={.kind=DS_RECT,.bounds={0,0,10,10},.clip={0,0,240,135},.opacity=255};
    d.data.shape.color=0xffffffff;return d;
}
int main(void){
    ds_core core;ds_core_init(&core);
    ds_client app=ds_core_client(&core,DS_APP),system=ds_core_client(&core,DS_SYSTEM);
    ds_tx tx,old_tx;ds_ref ref,old_ref;ds_draw d=rectangle();
    EXPECT(system.ops->begin(system.ctx,DS_REPLACE,&tx)==DS_OK);
    EXPECT(app.ops->add(app.ctx,tx,&d,&ref)==DS_STALE);
    app.ops->abort(app.ctx,tx);
    EXPECT(system.ops->add(system.ctx,tx,&d,&ref)==DS_OK);
    system.ops->abort(system.ctx,tx);
    EXPECT(app.ops->begin(app.ctx,DS_REPLACE,&old_tx)==DS_OK);
    EXPECT(app.ops->add(app.ctx,old_tx,&d,&old_ref)==DS_OK);
    app.ops->abort(app.ctx,old_tx);
    ds_core_init(&core);app=ds_core_client(&core,DS_APP);
    EXPECT(app.ops->begin(app.ctx,DS_REPLACE,&tx)==DS_OK);
    EXPECT(app.ops->add(app.ctx,tx,&d,&ref)==DS_OK);
    EXPECT(tx.value!=old_tx.value&&ref.value!=old_ref.value);
    app.ops->abort(app.ctx,tx);
    EXPECT(app.ops->begin(app.ctx,DS_PATCH,&tx)==DS_OK);
    EXPECT(app.ops->add(app.ctx,tx,&d,&ref)==DS_INVALID);
    app.ops->abort(app.ctx,tx);

    /* SYSTEM works before APP, and foreign end cannot seal its submission. */
    EXPECT(system.ops->begin(system.ctx,DS_REPLACE,&tx)==DS_OK);
    EXPECT(system.ops->add(system.ctx,tx,&d,&ref)==DS_OK);
    EXPECT(app.ops->end(app.ctx,tx)==DS_STALE);
    EXPECT(system.ops->end(system.ctx,tx)==DS_OK);
    ds_frame frame;ds_frame_command command;
    EXPECT(ds_core_frame(&core,&frame)==DS_OK&&frame.full_redraw);
    EXPECT(frame.next[DS_SYSTEM].commands==1&&frame.next_background==0x000000ff);
    EXPECT(ds_core_read(&core,frame.ticket,false,DS_SYSTEM,0,&command)==DS_OK);
    EXPECT(command.visible&&command.draw.data.shape.color==0xffffffff);
    EXPECT(ds_core_presented(&core,frame.ticket)==DS_OK);
    ds_tx stale=frame.ticket;
    EXPECT(ds_core_read(&core,stale,false,DS_SYSTEM,0,&command)==DS_STALE);

    /* Text is copied in both directions. Failure survives a discarded frame. */
    EXPECT(app.ops->begin(app.ctx,DS_REPLACE,&tx)==DS_OK);
    EXPECT(app.ops->background(app.ctx,tx,0x112233ff)==DS_OK);
    char input[]="abc";
    d.kind=DS_TEXT;
    d.data.text.utf8=input;d.data.text.bytes=3;d.data.text.capacity=8;
    d.data.text.font=DS_BODY;d.data.text.color=0xffffffff;
    EXPECT(app.ops->add(app.ctx,tx,&d,&ref)==DS_OK);
    input[0]='z';
    EXPECT(app.ops->end(app.ctx,tx)==DS_OK);
    EXPECT(ds_core_frame(&core,&frame)==DS_OK&&!frame.full_redraw);
    EXPECT(ds_core_presented(&core,stale)==DS_STALE);
    EXPECT(ds_core_discard(&core,stale)==DS_STALE);
    EXPECT(ds_core_read(&core,frame.ticket,false,DS_APP,0,&command)==DS_OK);
    EXPECT(command.draw.data.text.utf8==command.text&&memcmp(command.text,"abc",3)==0);
    EXPECT(ds_core_presented(&core,frame.ticket)==DS_OK);
    EXPECT(app.ops->begin(app.ctx,DS_PATCH,&tx)==DS_OK);
    ds_change change={.property=DS_SET_TEXT,.value.text={"xy",2}};
    EXPECT(app.ops->change(app.ctx,tx,ref,&change)==DS_OK);
    EXPECT(app.ops->end(app.ctx,tx)==DS_OK);
    EXPECT(ds_core_frame(&core,&frame)==DS_OK);
    EXPECT(ds_core_read(&core,frame.ticket,true,DS_APP,0,&command)==DS_OK);
    EXPECT(command.draw.data.text.bytes==3&&memcmp(command.text,"abc",3)==0);
    EXPECT(ds_core_read(&core,frame.ticket,false,DS_APP,0,&command)==DS_OK);
    EXPECT(command.draw.data.text.bytes==2&&memcmp(command.text,"xy",2)==0);
    EXPECT(ds_core_failed(&core,frame.ticket)==DS_OK);
    EXPECT(ds_core_discard(&core,frame.ticket)==DS_OK);
    EXPECT(app.ops->begin(app.ctx,DS_PATCH,&tx)==DS_OK);
    EXPECT(app.ops->end(app.ctx,tx)==DS_OK);
    EXPECT(ds_core_frame(&core,&frame)==DS_OK&&frame.full_redraw);
    EXPECT(ds_core_presented(&core,frame.ticket)==DS_OK);

    /* Reject excessive lengths before inspecting the caller's bytes. */
    EXPECT(app.ops->begin(app.ctx,DS_PATCH,&tx)==DS_OK);
    change.value.text.utf8="x";change.value.text.bytes=UINT16_MAX;
    EXPECT(app.ops->change(app.ctx,tx,ref,&change)==DS_LIMIT);
    app.ops->abort(app.ctx,tx);
    EXPECT(app.ops->begin(app.ctx,DS_PATCH,&tx)==DS_OK);
    change.value.text.utf8=NULL;change.value.text.bytes=0;
    EXPECT(app.ops->change(app.ctx,tx,ref,&change)==DS_OK);
    EXPECT(app.ops->end(app.ctx,tx)==DS_OK);
    EXPECT(ds_core_frame(&core,&frame)==DS_OK);
    ds_core_init(&core);
    EXPECT(ds_core_presented(&core,frame.ticket)==DS_STALE);
    EXPECT(app.ops->begin(app.ctx,DS_REPLACE,&tx)==DS_OK);
    EXPECT(app.ops->background(app.ctx,tx,0x000000ff)==DS_OK);
    d=rectangle();EXPECT(app.ops->add(app.ctx,tx,&d,&old_ref)==DS_OK);
    EXPECT(app.ops->change(app.ctx,tx,ref,&change)==DS_STALE);
    app.ops->abort(app.ctx,tx);
    if(failures)return 1;
    puts("review regressions: PASS");return 0;
}
