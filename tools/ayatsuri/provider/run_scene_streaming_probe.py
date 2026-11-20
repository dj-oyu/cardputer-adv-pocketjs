"""Private host scene probe with isolated streaming candidate; no target admission.
Input header directory stays private. Materials are diagnostic placeholders;
node/unmasked morph routes may run, but rig/shrinkwrap/material semantics do not.
"""
import argparse,os,pathlib,shutil,subprocess,sys
p=argparse.ArgumentParser();p.add_argument('--blob',type=pathlib.Path,required=True);p.add_argument('--headers',type=pathlib.Path,required=True);p.add_argument('--out',type=pathlib.Path,required=True);p.add_argument('--sanitize',action='store_true');a=p.parse_args()
here=pathlib.Path(__file__).resolve().parent;root=here.parents[2];out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
cmd=[sys.executable,str(here/'test_provider_streaming.py'),'--out',str(out/'synthetic')]
if a.sanitize:cmd+=['--sanitize']
subprocess.run(cmd,check=True)
for name in ['geometry.h','provider_nodes.h','driver_nodes.h','driver_programs.h','scene_pose.h','morph_targets.h','rig_targets.h','wrap_targets.h']:
    source=a.headers/name
    if source.exists():shutil.copyfile(source,out/name)
s=(root/'tools/hone/native_geometry/scene_provider_probe.c').read_text()
def replace(old,new):
 global s
 if s.count(old)!=1:raise RuntimeError('private streaming probe anchor changed: '+old[:60])
 s=s.replace(old,new)
replace('typedef struct {hone_provider_context context;unsigned calls;} probe_context;', '''#include "ui/kasane/ksn_mesh_depth.h"
size_t stream_ayatsuri_model_instance_bytes(const ayatsuri_model*);
ayatsuri_model_status stream_ayatsuri_model_instance_init(const ayatsuri_model*,void*,size_t,const void*,uint32_t,ayatsuri_model_instance*);
ayatsuri_model_status stream_ayatsuri_model_instance_dispose(ayatsuri_model_instance*,const void*,uint32_t);
ayatsuri_model_status stream_ayatsuri_model_instance_targets(ayatsuri_model_instance*,const void*,uint32_t,const ayatsuri_model_targets*);
ayatsuri_model_status stream_candidate_frame(ayatsuri_model_instance*,const void*,uint32_t,const float*,const float*,ayatsuri_model_geometry_provider,void*,ayatsuri_skin_result*,ksn_mesh_snapshot*);
static int fail_node=-1;
typedef struct {hone_provider_context context;unsigned calls;} probe_context;''')
replace('ctx->calls++;return hone_native_provider', 'ctx->calls++;if((int)id==fail_node)return false;return hone_native_provider')
helper=r'''
/* Host-only generous packet capacity is NOT a target RAM allocation plan. */
#define STREAM_CAP (512u*1024u)
static ksn_mesh_snapshot stream_ready,stream_pending,stream_oracle;
static uint16_t pixels[240*135],reference_pixels[240*135];static float depth_rows[240];
static unsigned stream_frames,stream_failures;static size_t maximum_snapshot;
static void stream_render(const ksn_mesh_snapshot*s,uint16_t*p){for(unsigned j=0;j<240*135;j++)p[j]=17;for(unsigned y=0;y<135;y+=8)assert(ksn_mesh_render(s,p+y*240,y,135-y<8?135-y:8,depth_rows,1));}
static void stream_setup(void){
 ksn_mesh_snapshot *list[]={&stream_ready,&stream_pending,&stream_oracle};
 for(unsigned j=0;j<3;j++){list[j]->packets=(co_buffer){malloc(STREAM_CAP),STREAM_CAP,0,0};assert(list[j]->packets.data);list[j]->enabled=true;list[j]->work_limit=1000000000;}
}
static void stream_compare(ayatsuri_model_instance*baseline,ayatsuri_model_instance*stream,void*owner,const float*camera,const float*light,probe_context*ctx,ayatsuri_skin_result*slot,size_t expected_calls){
 stream_oracle.packets.size=stream_oracle.packets.packets=stream_oracle.work=0;
 uint32_t cursor=0;ayatsuri_model_packet packet;ayatsuri_model_status status;
 while((status=ayatsuri_model_instance_next(baseline,owner,1,&cursor,&packet))==AYATSURI_MODEL_OK){ayatsuri_model_mesh_view v;assert(ayatsuri_model_instance_mesh_view(baseline,owner,1,packet.node_id,&v)==AYATSURI_MODEL_OK);assert(ksn_mesh_append(&stream_oracle,v.patch,v.depth,packet.node_id));}
 assert(status==AYATSURI_MODEL_DONE);
 unsigned before=ctx->calls;
 assert(stream_candidate_frame(stream,owner,1,camera,light,provider,ctx,slot,&stream_pending)==AYATSURI_MODEL_OK);
 assert(ctx->calls-before==expected_calls);
 assert(stream_pending.packets.size==stream_oracle.packets.size&&!memcmp(stream_pending.packets.data,stream_oracle.packets.data,stream_oracle.packets.size));
 ksn_mesh_snapshot swap=stream_ready;stream_ready=stream_pending;stream_pending=swap;
 stream_render(&stream_ready,pixels);stream_render(&stream_oracle,reference_pixels);assert(!memcmp(pixels,reference_pixels,sizeof pixels));
 if(stream_ready.packets.size>maximum_snapshot)maximum_snapshot=stream_ready.packets.size;
 size_t used=stream_ready.packets.size;void*saved=malloc(used?used:1);assert(saved);memcpy(saved,stream_ready.packets.data,used);ksn_mesh_snapshot old=stream_ready;
 unsigned ids[256],n=0;for(unsigned j=0;j<ctx->context.node_count;j++)if(ctx->context.nodes[j].count)ids[n++]=j;assert(n==expected_calls&&n);
 unsigned at[3]={0,n/2,n-1};
 for(unsigned k=0;k<3;k++){fail_node=(int)ids[at[k]];assert(stream_candidate_frame(stream,owner,1,camera,light,provider,ctx,slot,&stream_pending)!=AYATSURI_MODEL_OK);assert(!memcmp(&old,&stream_ready,sizeof old)&&!memcmp(saved,stream_ready.packets.data,used));stream_failures++;}
 fail_node=-1;stream_pending.packets.capacity=1;
 status=stream_candidate_frame(stream,owner,1,camera,light,provider,ctx,slot,&stream_pending);assert((status!=AYATSURI_MODEL_OK)==(used!=0));assert(!memcmp(&old,&stream_ready,sizeof old)&&!memcmp(saved,stream_ready.packets.data,used));if(used)stream_failures++;stream_pending.packets.capacity=STREAM_CAP;
 free(saved);stream_frames++;
}
'''
replace('int main(int argc,char **argv){',helper+'\nint main(int argc,char **argv){')
replace(' assert(status==AYATSURI_MODEL_OK&&ctx.calls==expected_calls);',''' assert(status==AYATSURI_MODEL_OK&&ctx.calls==expected_calls);
 size_t stream_bytes=stream_ayatsuri_model_instance_bytes(&m);void*stream_arena=malloc(stream_bytes);assert(stream_arena);ayatsuri_model_instance streamed;
 assert(stream_ayatsuri_model_instance_init(&m,stream_arena,stream_bytes,&owner,1,&streamed)==AYATSURI_MODEL_OK);
 stream_setup();stream_compare(&inst,&streamed,&owner,camera,light,&ctx,slot,expected_calls);''')
replace('  assert(ayatsuri_model_instance_targets(&inst,&owner,1,&targets)==AYATSURI_MODEL_OK);','''  assert(ayatsuri_model_instance_targets(&inst,&owner,1,&targets)==AYATSURI_MODEL_OK);
  assert(stream_ayatsuri_model_instance_targets(&streamed,&owner,1,&targets)==AYATSURI_MODEL_OK);''')
replace('  assert(ctx.calls-before==expected_calls);','  assert(ctx.calls-before==expected_calls);\n  stream_compare(&inst,&streamed,&owner,camera,light,&ctx,slot,expected_calls);')
replace(' free(work);free(meshes);free(arena);free(blob);return 0;', ''' free(work);free(meshes);free(arena);
 assert(stream_ayatsuri_model_instance_dispose(&streamed,&owner,1)==AYATSURI_MODEL_OK);free(stream_arena);free(blob);
 /* Both model arenas AND deformed source/normal buffers are gone. */
 stream_render(&stream_ready,pixels);assert(!memcmp(pixels,reference_pixels,sizeof pixels));
#ifdef HONE_PROBE_WRAP
 printf("PASS private streaming frames=%u mesh_calls_per_frame=%zu arena=%zu->%zu snapshot_max=%zu rollback=%u packet_raster_bitwise=true ready_outlives_source=true materials=placeholder rig=sampled_transform wrap=applied runtime_ready=false\\n",stream_frames,expected_calls,arena_bytes,stream_bytes,maximum_snapshot,stream_failures);
#elif defined(HONE_PROBE_RIG_DRIVER)
 printf("PASS private streaming frames=%u mesh_calls_per_frame=%zu arena=%zu->%zu snapshot_max=%zu rollback=%u packet_raster_bitwise=true ready_outlives_source=true materials=placeholder rig=sampled_transform shrinkwrap=unapplied runtime_ready=false\\n",stream_frames,expected_calls,arena_bytes,stream_bytes,maximum_snapshot,stream_failures);
#else
 printf("PASS private streaming frames=%u mesh_calls_per_frame=%zu arena=%zu->%zu snapshot_max=%zu rollback=%u packet_raster_bitwise=true ready_outlives_source=true materials=placeholder rig_shrinkwrap=unapplied runtime_ready=false\\n",stream_frames,expected_calls,arena_bytes,stream_bytes,maximum_snapshot,stream_failures);
#endif
 free(stream_ready.packets.data);free(stream_pending.packets.data);free(stream_oracle.packets.data);return 0;''')
(out/'scene_stream.c').write_text(s)
sources=['main/ayatsuri/ayatsuri_model.c','main/ayatsuri/ayatsuri_visibility.c','main/ayatsuri/compact_opaque.c','main/ui/kasane/ksn_shiki_frame.c','main/ui/kasane/ksn_mesh_depth.c','tools/hone/source_package/native_wedge_lut_candidate.c','tools/hone/source_package/rigid_dq_candidate.c']
if (out/'wrap_targets.h').exists():
    if not (out/'rig_targets.h').exists():raise ValueError('wrap probe requires rig')
    sources.append('tools/hone/native_geometry/native_wrap_candidate.c')
binary=out/('scene-stream.exe' if os.name=='nt' else 'scene-stream')
cmd=[os.environ.get('CC','cc'),'-std=c11','-O2','-g','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-ffp-contract=off','-fno-fast-math','-DAYATSURI_MODEL_EXPERIMENTAL_PROVIDER','-I'+str(out),'-I'+str(root/'tools/hone/native_geometry'),'-I'+str(root/'tools/hone/source_package'),'-I'+str(root/'main'),'-I'+str(root/'main/ayatsuri'),'-I'+str(root/'tools/hostshim'),*[str(root/s) for s in sources],str(out/'synthetic/stream_model.c'),str(out/'scene_stream.c'),'-lm','-o',str(binary)]
if (out/'driver_nodes.h').exists():cmd.insert(1,'-DHONE_PROBE_NODE_DRIVER')
if (out/'morph_targets.h').exists():cmd.insert(1,'-DHONE_PROBE_MORPH_DRIVER')
if (out/'rig_targets.h').exists():cmd.insert(1,'-DHONE_PROBE_RIG_DRIVER')
if (out/'wrap_targets.h').exists():cmd.insert(1,'-DHONE_PROBE_WRAP')
if a.sanitize:cmd[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
subprocess.run(cmd,check=True);subprocess.run([str(binary),str(a.blob.resolve())],check=True)
