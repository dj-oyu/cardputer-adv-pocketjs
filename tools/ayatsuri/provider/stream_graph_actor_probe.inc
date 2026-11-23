/* Included only by the host kobako harness. Caller owns the frame and expected
 * pixels; no private assets are embedded here. Copied-DMA completion is simulated. */
#include "tools/ayatsuri/provider/stream_graph_occlusion_backend.h"
static bool graph_actor_registered;
static hone_graph_band_work graph_actor_work;
static uint16_t graph_actor_row[240];
static float graph_actor_depth[240],graph_actor_port_depth[240],graph_actor_scratch[1024];
static unsigned graph_actor_frames;
void private_graph_actor_present(ayatsuri_stream_frame_owner*f,const uint16_t*expected){
 if(!graph_actor_registered){
  assert(kobako_kasane_reset());kobako_kasane_prepare();diag_frame=f;
  graph_actor_work=(hone_graph_band_work){f->identity,f->generation,graph_actor_row,graph_actor_depth,graph_actor_scratch,1024};
  kobako_stream_material_hooks hooks=ay_stream_graph_occlusion_hooks(&graph_actor_work);
  assert(kobako_kasane_stream_actor_bind_material(f,f->identity,f->generation,graph_actor_port_depth,&hooks)==KSN_OK);
  graph_actor_registered=true;
 }
 assert(diag_frame==f&&f->produced&&!f->pinned);const void*payload=f->payload[1-f->committed];
 ksn_display_port port={NULL,get_strip,diag_send,240,135,8,NULL,NULL};ksn_display_completion done={NULL,diag_finish};ksn_render_stats stats;
 diag_drain_fail=diag_send_fail=diag_reject=diag_grid_pending=false;diag_sends=0;
 assert(kobako_kasane_present_complete(&port,&done,&stats)==KSN_OK);
 assert(!f->pinned&&!f->produced&&!diag_dma&&f->payload[f->committed]==payload);
 assert(!memcmp(panel_pixels,expected,240*135*sizeof *expected));graph_actor_frames++;
}
void private_graph_actor_end(void){
 if(graph_actor_registered){ksn_display_completion done={NULL,diag_finish};assert(kobako_kasane_stream_actor_unbind(&done)==KSN_OK);assert(kobako_kasane_reset());graph_actor_registered=false;diag_frame=NULL;}
 printf("REAL_GRAPH_ACTOR frames=%u full_panel_pixel_equal=true joint_payload_ACK=true DMA=simulated lighting=false near_clip=false\n",graph_actor_frames);
}
