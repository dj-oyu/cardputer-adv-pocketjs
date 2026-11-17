#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#include "tools/hone/native_geometry/alpha_graph_band_candidate.h"
#include "tools/hone/native_geometry/alpha_graph_clip_candidate.h"
#pragma GCC diagnostic pop
static bool material_preflight(void*ctx,const ksn_mesh_snapshot*s,const void*p){
 return hone_graph_band_preflight(s,p,ctx)!=0;
}
static ksn_result material_band(void*ctx,const ksn_mesh_snapshot*s,const void*p,
 unsigned y,unsigned rows,uint16_t*pixels,float*depth){
 (void)depth;return hone_graph_band_render(s,p,ctx,y,rows,pixels)?KSN_OK:KSN_INVALID;
}
static void material_expected(const ksn_mesh_snapshot*opaque,const hone_graph_band_payload*p){
 static uint16_t expected[240*135];static float all_depth[240*135],scratch[16];
 for(unsigned i=0;i<240*135;i++)expected[i]=0;
 for(unsigned y=0;y<135;y++)assert(ksn_mesh_render(opaque,expected+y*240,y,1,all_depth+y*240,1));
 assert(hone_graph_render(&p->graph,p->binding,p->generation,expected,all_depth,240,135,8,240,scratch,16));
 assert(!memcmp(expected,panel_pixels,sizeof expected));
}
static void stream_material_tests(void){
 assert(pocket_kasane_reset());pocket_kasane_prepare();
 ayatsuri_model model;assert(ayatsuri_stream_candidate_open(ayatsuri_stream_probe_blob,sizeof ayatsuri_stream_probe_blob,&model)==AYATSURI_MODEL_OK);
 size_t bytes=ayatsuri_stream_candidate_bytes(&model);void*arena=malloc(bytes);unsigned char*packets=malloc(32768);assert(arena&&packets);
 ayatsuri_stream_candidate stream={0};int id=2;
 assert(ayatsuri_stream_candidate_init(&model,arena,bytes,&id,3,&stream)==AYATSURI_MODEL_OK);
 ayatsuri_stream_frame_owner frame={0};diag_frame=&frame;
 assert(ayatsuri_stream_frame_owner_init(&frame,&stream,&id,3,packets,16384,packets+16384,16384,1000000));
 static const hone_alpha_instruction acode[]={{.op=HONE_ALPHA_X},{.op=HONE_ALPHA_MUL,.a=0,.b=0}};
 static const hone_alpha_instruction red[]={{.op=HONE_ALPHA_DRIVER,.a=0}},green[]={{.op=HONE_ALPHA_CONST,.value=.4f}},blue[]={{.op=HONE_ALPHA_CONST,.value=.1f}};
 const hone_alpha_program alpha={acode,2,1},color[3]={{red,1,0},{green,1,0},{blue,1,0}};
 hone_graph_triangle input={.v={{-10,20,10000,{0,0,0}},{60,20,10000,{1,0,0}},{20,90,10000,{.5f,1,0}}},.alpha=&alpha,.color=color};
 hone_graph_triangle clipped[6];size_t count=0;assert(hone_graph_clip_viewport(&input,0,0,240,135,clipped,6,&count)&&count==2);
 hone_graph_packet graph_packets[6];for(size_t i=0;i<count;i++)graph_packets[i]=(hone_graph_packet){clipped[i],(uint32_t)i};
 const double driver_a[]={.8},driver_b[]={.2};
 hone_graph_band_payload pa={.graph={graph_packets,count,6,driver_a,1,&id,3,1},.binding=&id,.generation=3,.span_limit=240};
 hone_graph_band_payload pb=pa;pb.graph.drivers=driver_b;
 uint16_t row[240];float depth[240],band_depth[240],scratch[16];
 hone_graph_band_work work={&id,3,row,depth,scratch,16};
 pocket_stream_material_hooks hooks={&work,material_preflight,material_band};
 ayatsuri_skin_result slot[32];const float camera[9]={0,0,400,0,0,0,1,120,67},light[6]={0,0,1,1,1,1};
 assert(ayatsuri_stream_frame_produce(&frame,&id,3,camera,light,diag_provider,NULL,slot)==AYATSURI_MODEL_OK);
 assert(ayatsuri_stream_frame_payload(&frame,&id,3,&pa));
 assert(pocket_kasane_stream_actor_bind_material(&frame,&id,3,band_depth,&hooks)==KSN_OK);
 ksn_display_port port={NULL,get_strip,diag_send,240,135,8,NULL,NULL};ksn_display_completion done={NULL,diag_finish};ksn_render_stats stats;
 diag_drain_fail=diag_send_fail=diag_reject=diag_grid_pending=false;diag_sends=0;
 assert(pocket_kasane_present_complete(&port,&done,&stats)==KSN_OK);
 const ksn_mesh_snapshot*ready=ayatsuri_stream_frame_ready(&frame,&id,3);assert(ready);material_expected(ready,&pa);
 /* Frozen old material survives generation of a new geometry+driver frame. */
 assert(ayatsuri_stream_frame_produce(&frame,&id,3,camera,light,diag_provider,NULL,slot)==AYATSURI_MODEL_OK);
 assert(ayatsuri_stream_frame_payload(&frame,&id,3,&pb));
 diag_sends=0;done.finish=diag_finish_after_submit;
 assert(pocket_kasane_present_complete(&port,&done,&stats)==KSN_BUSY&&frame.pinned);
 assert(!ayatsuri_stream_frame_payload(&frame,&id,3,&pa));
 assert(frame.payload[frame.committed]==&pa&&frame.payload[1-frame.committed]==&pb);
 done.finish=diag_finish;assert(!pocket_kasane_reset());diag_drain_fail=false;
 /* Normal entry drains, rejects the failed candidate, and redraws old material. */
 assert(pocket_kasane_present_complete(&port,&done,&stats)==KSN_OK);
 assert(frame.payload[frame.committed]==&pa);material_expected(ayatsuri_stream_frame_ready(&frame,&id,3),&pa);
 assert(ayatsuri_stream_frame_produce(&frame,&id,3,camera,light,diag_provider,NULL,slot)==AYATSURI_MODEL_OK);
 assert(ayatsuri_stream_frame_payload(&frame,&id,3,&pb));
 assert(pocket_kasane_present_complete(&port,&done,&stats)==KSN_OK&&frame.payload[frame.committed]==&pb);
 material_expected(ayatsuri_stream_frame_ready(&frame,&id,3),&pb);
 /* Wrong package binding refuses before the first send and preserves READY. */
 hone_graph_band_payload bad=pa;bad.generation=4;
 assert(ayatsuri_stream_frame_produce(&frame,&id,3,camera,light,diag_provider,NULL,slot)==AYATSURI_MODEL_OK);
 assert(ayatsuri_stream_frame_payload(&frame,&id,3,&bad));diag_sends=0;
 assert(pocket_kasane_present_complete(&port,&done,&stats)!=KSN_OK&&!diag_sends);
 assert(frame.payload[frame.committed]==&pb);
 assert(pocket_kasane_stream_actor_unbind(&done)==KSN_OK);assert(pocket_kasane_reset());
 assert(ayatsuri_stream_candidate_dispose(&stream,&id,3)==AYATSURI_MODEL_OK);free(arena);free(packets);diag_frame=NULL;
 puts("STREAM_MATERIAL PASS actual_actor clipped_local_xyz nonlinear_alpha frozen_driver_payload ACK/rollback binding_gate unlit_synthetic_only");
}
