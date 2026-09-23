#include "core_fixture.h"
#include "app_legacy_presenter.h"
#include "ksn_view_host.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"presenter line %d: %s\n",__LINE__,#x);return 1;}}while(0)
KSN_TEST_CORE(core,static);
static ksn_cache cache;
static ksn_cache_command_block cache_commands;
static ksn_cache_text_block cache_text;

int main(void){
    ksn_presenter_values v={0};ksn_presenter_plan a,b;
    CHECK(ksn_presenter_copy_text(v.text[0],&v.bytes[0],"TRACK",5)==KSN_OK);
    CHECK(ksn_presenter_copy_text(v.text[1],&v.bytes[1],"PLAYING  1s",11)==KSN_OK);
    CHECK(ksn_presenter_make(KSN_PRESENTER_MUSIC,&v,240,135,&a)==KSN_OK);
    CHECK(a.count==7&&a.items[0].bounds.x0==8&&a.items[0].bounds.x1==46);
    CHECK(a.items[4].bounds.y0==109&&a.items[4].bounds.y1==111);
    v.duration_ms=100000;v.position_ms=1000;
    CHECK(ksn_presenter_make(KSN_PRESENTER_MUSIC,&v,240,135,&a)==KSN_OK&&a.count==8);
    v.position_ms=1001;
    CHECK(ksn_presenter_make(KSN_PRESENTER_MUSIC,&v,240,135,&b)==KSN_OK);
    CHECK(ksn_presenter_equal(&a,&b)); /* Milliseconds changed; no pixel did. */
    v.position_ms=2000;
    CHECK(ksn_presenter_make(KSN_PRESENTER_MUSIC,&v,240,135,&b)==KSN_OK);
    CHECK(!ksn_presenter_equal(&a,&b));
    v.duration_ms=0;v.playing=true;v.phase=18;
    CHECK(ksn_presenter_make(KSN_PRESENTER_MUSIC,&v,240,135,&a)==KSN_OK&&a.count==10);
    v.phase=0;
    CHECK(ksn_presenter_make(KSN_PRESENTER_MUSIC,&v,240,135,&b)==KSN_OK&&b.count==7);
    v.help=true;
    CHECK(ksn_presenter_make(KSN_PRESENTER_MUSIC,&v,240,135,&a)==KSN_OK&&a.count==8);
    CHECK(!ksn_presenter_equal(&a,&b));
    CHECK(ksn_presenter_copy_text(v.text[0],&v.bytes[0],"あ",3)==KSN_OK);
    v.help=false;v.playing=false;
    CHECK(ksn_presenter_make(KSN_PRESENTER_MUSIC,&v,240,135,&a)==KSN_OK);
    CHECK(a.items[0].bounds.x1==28); /* Legacy plate advance: 12 + 8. */
    CHECK(ksn_presenter_copy_text(v.text[0],&v.bytes[0],"\xf0\x9f\x98\x80",4)==KSN_OK);
    CHECK(ksn_presenter_make(KSN_PRESENTER_MUSIC,&v,240,135,&a)==KSN_OK);
    CHECK(a.items[0].bounds.x1==40); /* Two legacy UTF-16 advances: 24 + 8. */
    {
        char long_text[64];memset(long_text,'A',sizeof(long_text));
        CHECK(ksn_presenter_copy_text(v.text[0],&v.bytes[0],long_text,sizeof(long_text))==KSN_OK);
        CHECK(v.bytes[0]==KSN_PRESENTER_TEXT_MAX&&v.text[0][v.bytes[0]]==0);
    }
    CHECK(ksn_presenter_copy_text(v.text[0],&v.bytes[0],"\n",1)==KSN_INVALID);
    CHECK(ksn_presenter_copy_text(v.text[0],&v.bytes[0],"\xed\xa0\x80",3)==KSN_INVALID);
    CHECK(ksn_presenter_copy_text(v.text[0],&v.bytes[0],"\xe0\x80\x80",3)==KSN_INVALID);
    CHECK(ksn_cache_bind(&cache,&cache_commands,&cache_text)==KSN_OK);
    ksn_view_host host;ksn_view_host_init(&host,&core,&cache,17);
    ksn_view *app=ksn_view_host_endpoint(&host,KSN_APP);ksn_tx ticket;
    ksn_ref refs[KSN_PRESENTER_ITEMS]={0};
    CHECK(ksn_presenter_submit(app,(ksn_rect){0,0,240,135},(ksn_resource){0},&a,refs,&ticket)==KSN_OK);
    ksn_frame frame;CHECK(ksn_core_frame(&core,&frame)==KSN_OK);
    CHECK(frame.next[KSN_APP].commands==a.count);
    CHECK(ksn_view_poll(app).ticket.value==ticket.value&&
          ksn_view_poll(app).status==KSN_SUBMITTED);
    CHECK(ksn_view_cancel(app,ticket)==KSN_OK);
    puts("presenter: PASS");return 0;
}
