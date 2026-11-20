"""Host execution of the exact CPU core used by the opt-in device probe."""
import argparse,os,pathlib,subprocess
p=argparse.ArgumentParser();p.add_argument('--out',type=pathlib.Path,required=True);p.add_argument('--sanitize',action='store_true');a=p.parse_args()
root=pathlib.Path(__file__).resolve().parents[3];out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
cfile=out/'test.c';cfile.write_text(r'''#include "ayatsuri/ayatsuri_stream_probe_core.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint64_t ticks;static unsigned yields;static uint32_t hashes[32];static ayatsuri_stream_probe_stats*observed;
static ayatsuri_stream_probe_work*observed_work;
static uint16_t rows1[240*135],rows8[240*135];static float compare_depth[240*8];
static uint64_t clock_now(void*context){(void)context;return ++ticks;}
static void frame_yield(void*context,unsigned frame){
 (void)context;assert(frame==yields);hashes[frame]=observed->final_pixel_hash;
 /* This fixed public fixture always emits three packets. The core swaps its
  * two work buffers after each successful frame, before this callback. */
 unsigned char*data=(frame&1)?observed_work->ready_bytes:observed_work->pending_bytes;
 size_t used=0;for(unsigned i=0;i<3;i++){co_view v;assert(co_read(data+used,AY_STREAM_PROBE_PACKET_CAPACITY-used,&v));used+=v.size;}
 ksn_mesh_snapshot s={.packets={data,AY_STREAM_PROBE_PACKET_CAPACITY,used,3},.enabled=true};
 for(unsigned i=0;i<240*135;i++)rows1[i]=rows8[i]=17;
 for(unsigned y=0;y<135;y+=8){unsigned n=135-y<8?135-y:8;
  assert(ksn_mesh_render(&s,rows1+y*240,y,n,compare_depth,1));
  assert(ksn_mesh_render(&s,rows8+y*240,y,n,compare_depth,8));
 }
 assert(!memcmp(rows1,rows8,sizeof rows1));yields++;
}
int main(void){
 size_t bytes=ayatsuri_stream_probe_arena_bytes();assert(bytes>0);void*arena=malloc(bytes);assert(arena);
 ayatsuri_stream_probe_work*w=malloc(sizeof *w);assert(w);ayatsuri_stream_probe_stats stats;
 observed=&stats;observed_work=w;
 assert(!ayatsuri_stream_probe_run(arena,bytes-1,w,&stats,clock_now,frame_yield,NULL));assert(yields==0);
 assert(!ayatsuri_stream_probe_run(arena,bytes,(void*)arena,&stats,clock_now,frame_yield,NULL));
 assert(ayatsuri_stream_probe_run(arena,bytes,w,&stats,clock_now,frame_yield,NULL));
 assert(stats.frames==32&&stats.provider_calls==160&&stats.rollbacks==32&&yields==32);
 assert(stats.geometry_us==32&&stats.geometry_min_us==1&&stats.geometry_max_us==1&&stats.raster_us==32&&stats.ready_after_arena_wipe);
 assert(stats.final_pixel_hash==0xb0d19ffdu);
 printf("FRAME_HASHES");for(unsigned i=0;i<32;i++)printf(" %08x",hashes[i]);puts("");
 printf("SYNTH_PROBE_CORE PASS frames=%u rollback=%u arena=%zu workspace=%zu snapshot=%zu pixel_hash=%08x time=mock DMA=unexercised\n",stats.frames,stats.rollbacks,stats.arena_bytes,stats.workspace_bytes,stats.snapshot_peak,stats.final_pixel_hash);
 free(w);free(arena);return 0;
}
''')
sources=['main/ayatsuri/ayatsuri_model_stream_candidate.c','main/ayatsuri/ayatsuri_stream_probe_core.c','main/ayatsuri/ayatsuri_visibility.c','main/ayatsuri/compact_opaque.c','main/ui/kasane/ksn_shiki_frame.c','main/ui/kasane/ksn_mesh_depth.c']
reference=None
for optimization in ('-O0','-O2'):
 for rows in (1,8):
  binary=out/(f'probe-{optimization[2:]}-{rows}'+('.exe' if os.name=='nt' else ''))
  cmd=[os.getenv('CC','cc'),'-std=c11',optimization,f'-DAY_STREAM_PROBE_DEPTH_ROWS={rows}','-Wall','-Wextra','-Werror','-ffp-contract=off','-fno-fast-math','-I'+str(root/'main'),'-I'+str(root/'tools/hostshim'),*[str(root/s) for s in sources],str(cfile),'-lm','-o',str(binary)]
  if a.sanitize:cmd[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
  subprocess.run(cmd,check=True)
  result=subprocess.run([str(binary)],check=True,text=True,capture_output=True)
  hashes=next(line for line in result.stdout.splitlines() if line.startswith('FRAME_HASHES '))
  if reference is None:reference=hashes
  assert hashes==reference, f'frame hash mismatch: {optimization} rows={rows}'
  print(f'{optimization} depth_rows={rows}: '+result.stdout.splitlines()[-1])
print('PASS all 32 frame pixels rows1==rows8; hashes identical across O0/O2 and depth rows 1/8 (host, not timing)')
