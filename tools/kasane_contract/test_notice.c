#include "core_fixture.h"
#include "ksn_view_host.h"
#include "app_notice.h"
#include "ksn_indicator.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint16_t panel[240*135],strip[240*8];
static int fail=-1;
static uint16_t *buffer(void *ctx){(void)ctx;return strip;}
static ksn_result send(void *ctx,uint16_t y,uint16_t rows,const uint16_t *pixels){
    (void)ctx;if(y/8==fail)return KSN_IO;
    memcpy(panel+y*240,pixels,rows*240*2);return KSN_OK;
}
static ksn_result text(void *ctx,const ksn_draw *d,uint16_t reveal,int x,int y,unsigned n,uint8_t *out){
    (void)ctx;(void)reveal;(void)x;(void)y;
    if(n)memset(out,d->data.text.utf8[0]=='A'?255:0,n);
    return KSN_OK;
}
int main(void){
    KSN_TEST_CORE(core,);ksn_view_host host;ksn_view_host_init(&host,&core,NULL,0);
    ksn_view *app=ksn_view_host_endpoint(&host,KSN_APP),*system=ksn_view_host_endpoint(&host,KSN_SYSTEM);
    ksn_text_port font={.span=text};ksn_display_port port={NULL,buffer,send,240,135,8,&font,NULL};ksn_render_stats stats;
    ksn_tx app_tx,tx;assert(ksn_view_begin(app,KSN_REPLACE,&app_tx)==KSN_OK);
    assert(ksn_view_background(app,app_tx,0x0000ffff)==KSN_OK);
    assert(ksn_view_submit(app,app_tx)==KSN_OK);
    assert(ksn_view_begin(system,KSN_REPLACE,&tx)==KSN_BUSY);
    assert(ksn_view_host_present(&host,&port,&stats)==KSN_OK);
    sys_notice notice={.id=1,.owner=1,.phase=NOTICE_ACTIVE,.label="ALERT"};
    assert(ksn_view_begin(system,KSN_REPLACE,&tx)==KSN_OK);
    assert(ksn_notice_emit(system,tx,&notice,(ksn_resource){0},0)==KSN_OK);
    assert(ksn_view_submit(system,tx)==KSN_OK);notice.label[0]='B';
    fail=1;assert(ksn_view_host_present(&host,&port,&stats)==KSN_IO);
    assert(ksn_view_poll(system).status==KSN_SUBMITTED);
    fail=-1;assert(ksn_view_host_present(&host,&port,&stats)==KSN_OK);
    assert(ksn_view_poll(system).status==KSN_PRESENTED);
    assert(panel[10*240+42]!=panel[0]&&panel[50*240]==31);
    assert(ksn_view_poll(app).ticket.value==app_tx.value);
    assert(ksn_view_get_stats(system).displayed.commands==4);
    assert(ksn_view_get_stats(system).displayed.text_bytes==41);
    assert(ksn_view_begin(system,KSN_REPLACE,&tx)==KSN_OK);
    assert(ksn_notice_emit(system,tx,NULL,(ksn_resource){0},0)==KSN_OK);
    assert(ksn_view_submit(system,tx)==KSN_OK);
    assert(ksn_view_host_present(&host,&port,&stats)==KSN_OK);
    assert(panel[0]==31&&panel[10*240+42]==31);
    /* Check every pixel of the legacy geometry for all cell/clip states.
     * Compose over the notice, so removal must restore the banner as well. */
    for(unsigned lit=0;lit<=6;lit++)for(unsigned clipping=0;clipping<2;clipping++){
        assert(ksn_view_begin(system,KSN_REPLACE,&tx)==KSN_OK);
        assert(ksn_notice_emit(system,tx,&notice,(ksn_resource){0},0)==KSN_OK);
        assert(ksn_recording_emit(system,tx,(ksn_recording_indicator){true,lit,clipping})==KSN_OK);
        assert(ksn_view_submit(system,tx)==KSN_OK);
        assert(ksn_view_host_present(&host,&port,&stats)==KSN_OK);
        assert(ksn_view_get_stats(system).displayed.commands==12);
        for(int y=0;y<15;y++)for(int x=216;x<240;x++){
            uint16_t expected=0x1041; /* RGB565(16,8,8) */
            if(x>=226&&x<234&&y>=4&&y<12)expected=0xe986;
            if(x>=219&&x<223&&y<=10&&!(y&1)){
                unsigned cell=5-(unsigned)y/2;
                expected=cell<lit?(clipping&&cell==5?0xfd80:0xd6fc):0x2924;
            }
            assert(panel[y*240+x]==expected);
        }
    }
    assert(ksn_view_begin(system,KSN_REPLACE,&tx)==KSN_OK);
    assert(ksn_recording_emit(system,tx,(ksn_recording_indicator){true,7,false})==KSN_INVALID);
    assert(ksn_view_cancel(system,tx)==KSN_OK);
    assert(ksn_view_begin(system,KSN_REPLACE,&tx)==KSN_OK);
    assert(ksn_notice_emit(system,tx,&notice,(ksn_resource){0},0)==KSN_OK);
    assert(ksn_recording_emit(system,tx,(ksn_recording_indicator){0})==KSN_OK);
    assert(ksn_view_submit(system,tx)==KSN_OK);
    assert(ksn_view_host_present(&host,&port,&stats)==KSN_OK);
    assert(panel[226]==panel[0]&&ksn_view_get_stats(system).displayed.commands==4);
    puts("KASANE_NOTICE_OK APP busy, copied text, IO retry, quota, removal, independent outcome");
}
