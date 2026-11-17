/* Included by the retained real pocket/QuickJS host harness. Synthetic only. */
#include <assert.h>
#include <math.h>
#include "pocket/pocket_stream_candidate.h"
#include "ayatsuri/ayatsuri_stream_probe_fixture.h"
static bool diag_grid_pending;
bool pocket_grid_pending(void){return diag_grid_pending;}
static unsigned diag_sends,diag_finishes;
static bool diag_send_fail,diag_drain_fail,diag_reject;
static uint16_t diag_tx[240*8];static unsigned diag_y,diag_rows;
static bool diag_dma;
static ayatsuri_stream_frame_owner*diag_frame;
static ksn_result diag_send(void*ctx,uint16_t y,uint16_t rows,const uint16_t*p){
 (void)ctx;assert(diag_frame->pinned);diag_sends++;
 if(diag_dma)memcpy(panel_pixels+diag_y*240,diag_tx,diag_rows*240*2);
 memcpy(diag_tx,p,rows*240*2);diag_y=y;diag_rows=rows;diag_dma=true;
 return diag_send_fail&&y>=8?KSN_INVALID:KSN_OK;
}
static ksn_result diag_finish(void*ctx){
 (void)ctx;diag_finishes++;
 if(diag_dma)assert(diag_frame->pinned);
 if(diag_drain_fail)return KSN_BUSY;
 if(diag_dma)memcpy(panel_pixels+diag_y*240,diag_tx,diag_rows*240*2);
 diag_dma=false;return KSN_OK;
}
static bool diag_provider(void*ctx,unsigned node,unsigned count,ayatsuri_skin_result*slot){
 (void)ctx;(void)node;
 for(unsigned j=0;j<count;j++){float angle=j*6.283185307f/count;
 slot[j]=(ayatsuri_skin_result){{8*cosf(angle),8*sinf(angle),0},{0,0,1}};}
 return true;
}
static bool diag_preflight(void*ctx,const ksn_mesh_snapshot*s){
 (void)ctx;float z[240];uint16_t p[240];
 for(unsigned y=0;y<135;y++){memset(p,0,sizeof p);assert(ksn_mesh_render(s,p,y,1,z,1));}
 return !diag_reject;
}
static ksn_result diag_finish_after_submit(void*ctx){
 if(diag_sends)diag_drain_fail=true;
 return diag_finish(ctx);
}
static void stream_diag_tests(void){
 assert(pocket_kasane_reset());ksn_view*view;
 assert(ksn_runtime_system_acquire(&view)==KSN_OK);ksn_tx tx;
 assert(ksn_view_begin(view,KSN_REPLACE,&tx)==KSN_OK);assert(ksn_view_submit(view,tx)==KSN_OK);
 ayatsuri_model m;assert(ayatsuri_stream_candidate_open(ayatsuri_stream_probe_blob,sizeof ayatsuri_stream_probe_blob,&m)==AYATSURI_MODEL_OK);
 size_t n=ayatsuri_stream_candidate_bytes(&m);void*arena=malloc(n);assert(arena);
 ayatsuri_stream_candidate stream={0};int id=1;
 assert(ayatsuri_stream_candidate_init(&m,arena,n,&id,1,&stream)==AYATSURI_MODEL_OK);
 unsigned char*bytes=malloc(32768);assert(bytes);ayatsuri_stream_frame_owner f={0};diag_frame=&f;
 assert(ayatsuri_stream_frame_owner_init(&f,&stream,&id,1,bytes,16384,bytes+16384,16384,1000000));
 ayatsuri_skin_result slot[32];float depth[240];pocket_stream_candidate bridge={0};
 const float camera[9]={0,0,400,0,0,0,1,120,67},light[6]={0,0,1,1,1,1};
 ksn_display_port port={NULL,get_strip,diag_send,240,135,8,NULL,NULL};
 ksn_display_completion completion={NULL,diag_finish};ksn_render_stats stats;
 for(unsigned attempt=0;attempt<5;attempt++){
  assert(ayatsuri_stream_frame_produce(&f,&id,1,camera,light,diag_provider,NULL,slot)==AYATSURI_MODEL_OK);
  const ksn_mesh_snapshot*old=ayatsuri_stream_frame_ready(&f,&id,1);
  diag_sends=0;diag_send_fail=attempt==1;diag_reject=attempt==3;diag_grid_pending=attempt==4;
  completion.finish=attempt==2?diag_finish_after_submit:diag_finish;
  ksn_result result=pocket_stream_candidate_present(&bridge,&f,&id,1,depth,
     diag_preflight,NULL,&port,&completion,&stats);
  if(attempt==0){
   assert(result==KSN_OK&&!bridge.held&&!diag_dma&&diag_sends==17);assert(f.ready);
   const ksn_mesh_snapshot*ready=ayatsuri_stream_frame_ready(&f,&id,1);
   uint16_t row[240];float z[240];
   for(unsigned y=0;y<135;y++){memset(row,0,sizeof row);assert(ksn_mesh_render(ready,row,y,1,z,1));assert(!memcmp(row,panel_pixels+y*240,sizeof row));}
  }
  else if(attempt==2){
   assert(result==KSN_BUSY&&bridge.held&&f.pinned&&diag_dma);
   assert(ayatsuri_stream_frame_produce(&f,&id,1,camera,light,diag_provider,NULL,slot)==AYATSURI_MODEL_STATE);
   completion.finish=diag_finish;
   assert(pocket_stream_candidate_recover(&bridge,&completion)==KSN_BUSY&&bridge.held);
   diag_drain_fail=false;
   assert(pocket_stream_candidate_recover(&bridge,&completion)==KSN_OK&&!bridge.held&&!f.pinned&&!diag_dma);
  }else {assert(result!=KSN_OK&&!bridge.held&&!f.pinned&&!diag_dma);if(attempt>=3)assert(!diag_sends);}
  if(attempt)assert(ayatsuri_stream_frame_ready(&f,&id,1)==old);
 }
 diag_grid_pending=false;diag_send_fail=false;diag_reject=false;
 assert(!bridge.held&&!f.pinned);
 assert(ayatsuri_stream_candidate_dispose(&stream,&id,1)==AYATSURI_MODEL_OK);
 free(arena);free(bytes);diag_frame=NULL;
 /* Drain recovery left normal runtime repair to its existing owner. */
 port.present=send_strip;fail_once=false;fail_band=-1;invalidate_band=-1;
 ksn_render_stats repair;assert(pocket_kasane_present_complete(&port,&completion,&repair)==KSN_OK);
 unsigned finished=diag_finishes;
 assert(pocket_kasane_present_complete(&port,&completion,&repair)==KSN_OK&&diag_finishes==finished+1);
 assert(ksn_runtime_shutdown()==KSN_OK);
 puts("STREAM_DISPLAY PASS actual_pocket_runtime synthetic_DMA success/send_failure/drain_failure/preflight/conflict rollback");
}
