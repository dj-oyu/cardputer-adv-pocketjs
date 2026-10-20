/* Fake elapsed clock with real coarse/detail scope implementations. */
#include "core_fixture.h"
#include "ksn_render.h"
#include "derby_measure.h"
#include "derby_detail.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
static uint64_t clock_us;
static bool pending,late_failure;
#ifdef KASANE_DMA_OVERLAP
static unsigned pending_y;
static int fail_completion_y=-1,fail_queue_y=-1;
static bool capture_flag,unknown_descriptor;
#endif
static uint16_t pixels[240*8];
typedef struct { unsigned sent_us; size_t pending_bytes; bool pending_capture; } kasane_display_t;
uint64_t dm_now_us(void){return clock_us;}
#ifdef KASANE_DMA_OVERLAP
static int64_t esp_timer_get_time(void){return (int64_t)clock_us;}
static bool board_capture_active(void){return capture_flag;}
static esp_err_t board_present_finish(void){
 if(!pending)return ESP_OK;
 DD_SCOPE(DD_TX_REAP);DD_ADD(DD_TX_REAPS,1);
 clock_us+=37;
 if(unknown_descriptor){DD_ADD(DD_TX_FAILURES,1);return 9;}
 pending=false;
 if((late_failure&&pending_y==128)||(int)pending_y==fail_completion_y){DD_ADD(DD_TX_FAILURES,1);return 7;}return ESP_OK;
}
#endif
#ifdef KASANE_DMA_OVERLAP
static esp_err_t board_present_submit(int x,int y,int cols,int rows,uint16_t *p){
 (void)x;(void)y;(void)p;
 assert(!pending);
 {DD_SCOPE(DD_TX_COPY);DD_ADD(DD_TX_COPY_BYTES,rows*cols*2);clock_us+=2;}
 {DD_SCOPE(DD_TX_SUBMIT);DD_ADD(DD_TX_SUBMITS,1);clock_us+=1;
  if(y==fail_queue_y){DD_ADD(DD_TX_FAILURES,1);return 8;}
  DD_ADD(DD_TX_PAYLOAD_BYTES,rows*cols*2);}
 pending_y=(unsigned)y;pending=true;return ESP_OK;
}
#endif
/* The runner inserts the actual app_session.c kasane_finish function here. */
DMA_FINISH_BOUNDARY
static uint16_t *strip(void *ctx){(void)ctx;return pixels;}
static ksn_result backdrop(void *ctx,uint16_t y,uint16_t rows,uint16_t *p){
 (void)ctx;(void)y;clock_us+=5;memset(p,0,rows*240*2);return KSN_OK;
}
static ksn_result send(void *ctx,uint16_t y,uint16_t rows,const uint16_t *p){
 DM_SCOPE(DM_TRANSFER);
#ifdef KASANE_DMA_OVERLAP
 return kasane_dma_submit(ctx,0,y,240,rows,(uint16_t *)p)==ESP_OK?KSN_OK:KSN_IO;
#else
 (void)ctx;(void)y;(void)p;
 {DD_SCOPE(DD_TX_COPY);DD_ADD(DD_TX_COPY_BYTES,rows*240*2);clock_us+=2;}
 {DD_SCOPE(DD_TX_SUBMIT);DD_ADD(DD_TX_SUBMITS,1);DD_ADD(DD_TX_PAYLOAD_BYTES,rows*240*2);clock_us+=1;}
 {DD_SCOPE(DD_TX_REAP);DD_ADD(DD_TX_REAPS,1);clock_us+=11;}
 dm_transfer(rows*240*2,0,0);return KSN_OK;
#endif
}

int main(void){
 for(unsigned failure=0;failure<2;failure++){
  KSN_TEST_CORE(core,);ksn_core_init(&core);ksn_client app=ksn_core_client(&core,KSN_APP);ksn_tx t;
  assert(app.ops->begin(app.ctx,KSN_REPLACE,&t)==KSN_OK);
  assert(app.ops->background(app.ctx,t,0x102030ff)==KSN_OK);
  assert(app.ops->end(app.ctx,t)==KSN_OK);
  kasane_display_t display={0};
  ksn_display_port port={.ctx=&display,.strip=strip,.present=send,.width=240,.height=135,.strip_rows=8};
  ksn_render_stats stats;dm_snapshot m;dd_snapshot d;
  clock_us=0;pending=false;late_failure=failure;dm_reset();dd_start();
#ifdef KASANE_DMA_OVERLAP
  const ksn_display_completion completion={&display,kasane_finish};
  ksn_result r=ksn_render_rects_complete(&core,&port,backdrop,false,&completion,&stats);
  assert(r==(failure?KSN_IO:KSN_OK));
  assert(ksn_core_poll(&core).status==(failure?KSN_SUBMITTED:KSN_PRESENTED));
  assert(display.sent_us==37);
  unsigned tx_time=17*3+17*37;
#else
  ksn_result r=ksn_render_rects_complete(&core,&port,backdrop,false,NULL,&stats);
  assert(r==KSN_OK&&ksn_core_poll(&core).status==KSN_PRESENTED);
  unsigned tx_time=17*14;
#endif
  assert(!pending&&stats.transferred_bytes==64800);
  assert(dm_read(&m)&&dd_finish(&d)&&!m.faults&&!d.faults);
  assert(m.stage[DM_RASTER].inclusive_us==tx_time+85);
  assert(m.stage[DM_TRANSFER].inclusive_us==tx_time&&m.stage[DM_TRANSFER].self_us==tx_time);
  assert(m.stage[DM_RASTER].self_us==85&&m.stage[DM_GUEST].self_us==0);
  assert(d.stage[DD_RENDER].inclusive_us==tx_time+85);
  assert(d.stage[DD_TX_SYNC].inclusive_us==tx_time);
  assert(d.stage[DD_TX_REAP].calls==17&&d.event[DD_TX_REAPS]==17);
  assert(d.event[DD_TX_COPY_BYTES]==64800&&d.event[DD_TX_PAYLOAD_BYTES]==64800&&d.event[DD_TX_SUBMITS]==17);
#ifdef KASANE_DMA_OVERLAP
  assert(m.transfer_bytes==(failure?61440:64800)&&m.transfer_success==(failure?16:17));
  assert(m.transfer_failures==failure&&d.event[DD_TX_FAILURES]==failure);
#else
  assert(m.transfer_bytes==64800&&m.transfer_success==17);
  assert(!m.transfer_failures&&!d.event[DD_TX_FAILURES]);
#endif
 }
#ifdef KASANE_DMA_OVERLAP
 for(unsigned failure=0;failure<3;failure++){
  kasane_display_t display={0};dm_snapshot m;dd_snapshot d;
  late_failure=false;pending=false;capture_flag=true;
  fail_completion_y=fail_queue_y=-1;clock_us=0;dm_reset();dd_start();
  assert(kasane_dma_submit(&display,0,0,240,8,pixels)==ESP_OK);
  capture_flag=false;
  if(failure==0)fail_completion_y=0;
  if(failure==1)fail_queue_y=8;
  esp_err_t r=kasane_dma_submit(&display,0,8,240,8,pixels);
  assert(r==(failure==0?7:failure==1?8:ESP_OK));
  assert(kasane_finish(&display)==KSN_OK&&!pending&&!display.pending_bytes);
  assert(kasane_finish(&display)==KSN_OK);
  assert(dm_read(&m)&&dd_finish(&d)&&!m.faults&&!d.faults);
  assert(m.transfer_success==(failure==0?0:failure==1?1:2));
  assert(m.transfer_bytes==(failure==0?0:failure==1?3840:7680));
  assert(m.transfer_failures==(failure<2?1:0)&&m.capture_samples==1);
  assert(d.event[DD_TX_FAILURES]==(failure<2?1:0));
  assert(d.event[DD_TX_PAYLOAD_BYTES]==(failure<2?3840:7680));
 }
#endif
#ifdef KASANE_DMA_OVERLAP
 /* Unknown descriptor failures retain DMA ownership, but settle the strip's
  * coarse outcome only once; repeated reap failures remain detail events. */
 {
  kasane_display_t display={0};dm_snapshot m;dd_snapshot d;
  pending=false;late_failure=false;capture_flag=false;
  fail_completion_y=fail_queue_y=-1;clock_us=0;dm_reset();dd_start();
  assert(kasane_dma_submit(&display,0,0,240,8,pixels)==ESP_OK);
  unknown_descriptor=true;
  assert(kasane_finish(&display)==KSN_IO&&pending&&!display.pending_bytes);
  assert(kasane_finish(&display)==KSN_IO&&pending&&!display.pending_bytes);
  unknown_descriptor=false;
  assert(kasane_finish(&display)==KSN_OK&&!pending);
  assert(kasane_finish(&display)==KSN_OK);
  assert(dm_read(&m)&&dd_finish(&d)&&!m.faults&&!d.faults);
  assert(m.transfer_success==0&&m.transfer_bytes==0&&m.transfer_failures==1);
  assert(d.event[DD_TX_FAILURES]==2&&d.event[DD_TX_REAPS]==3);
  assert(d.event[DD_TX_PAYLOAD_BYTES]==3840&&d.event[DD_TX_COPY_BYTES]==3840);
  assert(d.event[DD_TX_SUBMITS]==1&&display.sent_us==111);
 }
#endif
 puts("J1 DMA fake-clock accounting: final drain in raster/transfer inclusive; no raster/guest self leakage or duplicate bytes; late failure prevents ACK: PASS");
}
