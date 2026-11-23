#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#include "tools/ayatsuri/provider/stream_graph_occlusion_backend.h"
#include "tools/hone/native_geometry/alpha_graph_clip_candidate.h"
#pragma GCC diagnostic pop
static void material_expected(const ksn_mesh_snapshot*opaque,const hone_graph_band_payload*p){
 static uint16_t expected[240*135];static float all_depth[240*135],scratch[16];
 for(unsigned i=0;i<240*135;i++)expected[i]=0;
 for(unsigned y=0;y<135;y++)assert(ksn_mesh_render(opaque,expected+y*240,y,1,all_depth+y*240,1));
 /* Unrestricted reference, independent of the candidate collector. */
 for(unsigned y=0;y<135;y++)for(unsigned x=0;x<240;x++){
  hone_graph_ordered all[32];unsigned n=0;
  for(size_t j=0;j<p->graph.count;j++){
   hone_graph_fragment q;int hit=hone_graph_fragment_at(&p->graph.packets[j].triangle,x,y,all_depth[y*240+x],p->graph.drivers,p->graph.driver_count,scratch,16,&q);assert(hit>=0);if(!hit)continue;assert(n<32);
   hone_graph_ordered item={q,p->graph.packets[j].order};unsigned k=n++;
   while(k&&(all[k-1].fragment.z>q.z||(all[k-1].fragment.z==q.z&&all[k-1].order<=item.order))){all[k]=all[k-1];k--;}all[k]=item;
  }
  uint16_t original=expected[y*240+x];float rgb[3]={(original>>11)/31.f,((original>>5)&63)/63.f,(original&31)/31.f};
  for(unsigned j=0;j<n;j++)for(unsigned k=0;k<3;k++)rgb[k]=all[j].fragment.rgb[k]*all[j].fragment.alpha+rgb[k]*(1-all[j].fragment.alpha);
  expected[y*240+x]=(uint16_t)(((unsigned)floorf(rgb[0]*31+.5f)<<11)|((unsigned)floorf(rgb[1]*63+.5f)<<5)|(unsigned)floorf(rgb[2]*31+.5f));
 }
 assert(!memcmp(expected,panel_pixels,sizeof expected));
}
static void stream_material_tests(void){
 assert(kobako_kasane_reset());kobako_kasane_prepare();
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
 hone_graph_packet graph_packets[16];for(size_t i=0;i<count;i++)graph_packets[i]=(hone_graph_packet){clipped[i],(uint32_t)i};
 static const hone_alpha_instruction opaque_code[]={{.op=HONE_ALPHA_CONST,.value=1}};
 const hone_alpha_program opaque_alpha={opaque_code,1,0};size_t original_count=count;
 for(unsigned layer=0;layer<5;layer++)for(size_t j=0;j<original_count;j++){
  graph_packets[count]=graph_packets[j];graph_packets[count].triangle.alpha=&opaque_alpha;graph_packets[count].order=(uint32_t)count;
  for(unsigned k=0;k<3;k++){graph_packets[count].triangle.v[k].z=9999-layer;}
  count++;
 }
 const double driver_a[]={.8},driver_b[]={.2};
 hone_graph_band_payload pa={.graph={graph_packets,count,16,driver_a,1,&id,3,1},.binding=&id,.generation=3,.span_limit=240};
 hone_graph_band_payload pb=pa;pb.graph.drivers=driver_b;
 uint16_t row[240];float depth[240],band_depth[240],scratch[16];
 hone_graph_band_work work={&id,3,row,depth,scratch,16};
 kobako_stream_material_hooks hooks=ay_stream_graph_occlusion_hooks(&work);
 ayatsuri_skin_result slot[32];const float camera[9]={0,0,400,0,0,0,1,120,67},light[6]={0,0,1,1,1,1};
 assert(ayatsuri_stream_frame_produce(&frame,&id,3,camera,light,diag_provider,NULL,slot)==AYATSURI_MODEL_OK);
 assert(ayatsuri_stream_frame_payload(&frame,&id,3,&pa));
 assert(kobako_kasane_stream_actor_bind_material(&frame,&id,3,band_depth,&hooks)==KSN_OK);
 ksn_display_port port={NULL,get_strip,diag_send,240,135,8,NULL,NULL};ksn_display_completion done={NULL,diag_finish};ksn_render_stats stats;
 diag_drain_fail=diag_send_fail=diag_reject=diag_grid_pending=false;diag_sends=0;
 assert(kobako_kasane_present_complete(&port,&done,&stats)==KSN_OK);
 const ksn_mesh_snapshot*ready=ayatsuri_stream_frame_ready(&frame,&id,3);assert(ready);material_expected(ready,&pa);
 /* Frozen old material survives generation of a new geometry+driver frame. */
 assert(ayatsuri_stream_frame_produce(&frame,&id,3,camera,light,diag_provider,NULL,slot)==AYATSURI_MODEL_OK);
 assert(ayatsuri_stream_frame_payload(&frame,&id,3,&pb));
 diag_sends=0;done.finish=diag_finish_after_submit;
 assert(kobako_kasane_present_complete(&port,&done,&stats)==KSN_BUSY&&frame.pinned);
 assert(!ayatsuri_stream_frame_payload(&frame,&id,3,&pa));
 assert(frame.payload[frame.committed]==&pa&&frame.payload[1-frame.committed]==&pb);
 done.finish=diag_finish;assert(!kobako_kasane_reset());diag_drain_fail=false;
 /* Normal entry drains, rejects the failed candidate, and redraws old material. */
 assert(kobako_kasane_present_complete(&port,&done,&stats)==KSN_OK);
 assert(frame.payload[frame.committed]==&pa);material_expected(ayatsuri_stream_frame_ready(&frame,&id,3),&pa);
 assert(ayatsuri_stream_frame_produce(&frame,&id,3,camera,light,diag_provider,NULL,slot)==AYATSURI_MODEL_OK);
 assert(ayatsuri_stream_frame_payload(&frame,&id,3,&pb));
 assert(kobako_kasane_present_complete(&port,&done,&stats)==KSN_OK&&frame.payload[frame.committed]==&pb);
 material_expected(ayatsuri_stream_frame_ready(&frame,&id,3),&pb);
 /* Wrong package binding refuses before the first send and preserves READY. */
 hone_graph_band_payload bad=pa;bad.generation=4;
 assert(ayatsuri_stream_frame_produce(&frame,&id,3,camera,light,diag_provider,NULL,slot)==AYATSURI_MODEL_OK);
 assert(ayatsuri_stream_frame_payload(&frame,&id,3,&bad));diag_sends=0;
 assert(kobako_kasane_present_complete(&port,&done,&stats)!=KSN_OK&&!diag_sends);
 assert(frame.payload[frame.committed]==&pb);
 /* Invalid shader behind opaque coverage still fails before any send. */
 hone_graph_packet invalid_packets[16];memcpy(invalid_packets,graph_packets,sizeof graph_packets);
 const hone_alpha_instruction invalid_code[]={{.op=HONE_ALPHA_CONST,.value=NAN}};
 const hone_alpha_program invalid_color[3]={{invalid_code,1,0},{green,1,0},{blue,1,0}};
 invalid_packets[count-1].triangle.color=invalid_color;bad=pa;bad.graph.packets=invalid_packets;
 assert(ayatsuri_stream_frame_produce(&frame,&id,3,camera,light,diag_provider,NULL,slot)==AYATSURI_MODEL_OK);
 assert(ayatsuri_stream_frame_payload(&frame,&id,3,&bad));diag_sends=0;
 assert(kobako_kasane_present_complete(&port,&done,&stats)!=KSN_OK&&!diag_sends&&frame.payload[frame.committed]==&pb);
 assert(kobako_kasane_stream_actor_unbind(&done)==KSN_OK);assert(kobako_kasane_reset());
 assert(ayatsuri_stream_candidate_dispose(&stream,&id,3)==AYATSURI_MODEL_OK);free(arena);free(packets);diag_frame=NULL;
 puts("STREAM_OCCLUSION_MATERIAL PASS actual_actor clipped_local_xyz nonlinear_alpha frozen_driver_payload ACK/rollback binding_gate unlit_synthetic_only");
}
