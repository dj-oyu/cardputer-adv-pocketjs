"""Actual opaque renderer + row-depth alpha lane, synthetic fixtures only."""
import pathlib,argparse,subprocess,os
p=argparse.ArgumentParser();p.add_argument('--root',type=pathlib.Path,required=True);p.add_argument('--out',type=pathlib.Path,required=True);p.add_argument('--sanitize',action='store_true');a=p.parse_args();root=a.root.resolve();out=a.out.resolve();out.mkdir(parents=True,exist_ok=True);here=pathlib.Path(__file__).parent.resolve()
s=(root/'tools/ayatsuri/provider/test_provider.c').read_text();start=s.index('static void render(');end=s.index('static void entry_rejections',start)
s=s[:start]+'''
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#include "frame_lane_candidate.h"
#include <time.h>
static clock_t legacy_ticks,preflight_ticks,replay_ticks;
static unsigned timed_frames;
#pragma GCC diagnostic pop
static void render(const ksn_mesh_snapshot*s,uint16_t*out,unsigned band,unsigned rows){
 (void)rows;
 for(unsigned j=0;j<240*135;j++)out[j]=17;
 clock_t tick=clock();for(unsigned y=0;y<135;y+=band)assert(ksn_mesh_render(s,out+y*240,y,135-y<band?135-y:band,depthwork,1));legacy_ticks+=clock()-tick;
 unsigned owner=1;float z[240];
 hone_alpha_packet packets[5];packets[0]=(hone_alpha_packet){{{0,130,999,0},{4,130,999,4},{0,134,999,0}},{1,0,0},.37f,0,0,1,0,0};for(int i=1;i<5;i++)packets[i]=packets[0];
 hone_alpha_snapshot alpha={packets,1,5,&owner,1,0,1};
 hone_frame_lane candidate={s,&alpha,&owner,1,0,0},ready={s,&alpha,&owner,1,0,0};
 tick=clock();assert(hone_frame_preflight(&candidate,&owner,1,54));preflight_ticks+=clock()-tick;assert(hone_frame_commit(&ready,&candidate,&owner,1));
 hone_frame_lane old=ready;hone_alpha_snapshot bad=alpha;bad.count=5;candidate.alpha=&bad;assert(!hone_frame_preflight(&candidate,&owner,1,54));assert(!hone_frame_commit(&ready,&candidate,&owner,1));assert(!memcmp(&ready,&old,sizeof ready));candidate.alpha=&alpha;
 assert(hone_frame_preflight(&candidate,&owner,1,54));ready.pinned=1;assert(!hone_frame_commit(&ready,&candidate,&owner,1));ready.pinned=0;assert(hone_frame_commit(&ready,&candidate,&owner,1));ready.pinned=1;
 for(unsigned j=0;j<240*135;j++)out[j]=17;
 tick=clock();for(unsigned y=0;y<135;y+=band)assert(hone_frame_band(&ready,&owner,1,out+y*240,y,135-y<band?135-y:band,z));
 replay_ticks+=clock()-tick;timed_frames++;ready.pinned=0;
}
''' + s[end:];s=s.replace('__real_free(stack);}', '__real_free(stack);printf("host same opaque fixture row1 mean us/frame legacy=%.2f preflight=%.2f alpha_replay=%.2f frames=%u\\n",1e6*legacy_ticks/CLOCKS_PER_SEC/timed_frames,1e6*preflight_ticks/CLOCKS_PER_SEC/timed_frames,1e6*replay_ticks/CLOCKS_PER_SEC/timed_frames,timed_frames);}')
s=s.replace('for(unsigned j=0;j<240*135;j++)assert(pixels[j]==17);','for(unsigned j=0;j<240*135;j++)if(j/240<130||j%240>4)assert(pixels[j]==17);');(out/'test.c').write_text(s)
subprocess.run(['node',str(root/'tools/ayatsuri/provider/fixture.cjs'),str(out/'synthetic.aynm')],check=True);subprocess.run(['node',str(root/'tools/ayatsuri/skin/render_fixture.cjs'),str(out)],check=True)
src=['main/ayatsuri/ayatsuri_model.c','main/ayatsuri/ayatsuri_visibility.c','main/ayatsuri/compact_opaque.c','main/ui/kasane/ksn_shiki_frame.c','main/ui/kasane/ksn_mesh_depth.c'];cmd=['cc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-ffp-contract=off','-fno-fast-math','-DAYATSURI_MODEL_EXPERIMENTAL_PROVIDER','-I'+str(root/'main'),'-I'+str(root/'tools/hostshim'),'-I'+str(here),*[str(root/x) for x in src],str(out/'test.c'),'-lm','-pthread','-Wl,--wrap=malloc,--wrap=calloc,--wrap=free','-o',str(out/'test')]
if a.sanitize:cmd[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
subprocess.run(cmd,check=True);env=dict(os.environ);env['ASAN_OPTIONS']='detect_leaks=0:abort_on_error=1';subprocess.run([str(out/'test'),str(out/'synthetic.aynm')],check=True,env=env)
