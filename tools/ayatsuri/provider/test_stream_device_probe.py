"""Host execution of the exact CPU core used by the opt-in device probe."""
import argparse,os,pathlib,subprocess
p=argparse.ArgumentParser();p.add_argument('--out',type=pathlib.Path,required=True);p.add_argument('--sanitize',action='store_true');a=p.parse_args()
root=pathlib.Path(__file__).resolve().parents[3];out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
cfile=out/'test.c';cfile.write_text(r'''#include "ayatsuri/ayatsuri_stream_probe_core.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint64_t ticks;static unsigned yields;
static uint64_t clock_now(void*context){(void)context;return ++ticks;}
static void frame_yield(void*context,unsigned frame){(void)context;assert(frame==yields);yields++;}
int main(void){
 size_t bytes=ayatsuri_stream_probe_arena_bytes();assert(bytes>0);void*arena=malloc(bytes);assert(arena);
 ayatsuri_stream_probe_work*w=malloc(sizeof *w);assert(w);ayatsuri_stream_probe_stats stats;
 assert(!ayatsuri_stream_probe_run(arena,bytes-1,w,&stats,clock_now,frame_yield,NULL));assert(yields==0);
 assert(!ayatsuri_stream_probe_run(arena,bytes,(void*)arena,&stats,clock_now,frame_yield,NULL));
 assert(ayatsuri_stream_probe_run(arena,bytes,w,&stats,clock_now,frame_yield,NULL));
 assert(stats.frames==32&&stats.provider_calls==160&&stats.rollbacks==32&&yields==32);
 assert(stats.geometry_us==32&&stats.geometry_min_us==1&&stats.geometry_max_us==1&&stats.raster_us==32&&stats.ready_after_arena_wipe);
 printf("SYNTH_PROBE_CORE PASS frames=%u rollback=%u arena=%zu workspace=%zu snapshot=%zu pixel_hash=%08x time=mock DMA=unexercised\n",stats.frames,stats.rollbacks,stats.arena_bytes,stats.workspace_bytes,stats.snapshot_peak,stats.final_pixel_hash);
 free(w);free(arena);return 0;
}
''')
sources=['main/ayatsuri/ayatsuri_model_stream_candidate.c','main/ayatsuri/ayatsuri_stream_probe_core.c','main/ayatsuri/ayatsuri_visibility.c','main/ayatsuri/compact_opaque.c','main/ui/kasane/ksn_shiki_frame.c','main/ui/kasane/ksn_mesh_depth.c']
for optimization in ('-O0','-O2'):
 binary=out/('probe.exe' if os.name=='nt' else 'probe')
 cmd=[os.getenv('CC','cc'),'-std=c11',optimization,'-Wall','-Wextra','-Werror','-ffp-contract=off','-fno-fast-math','-I'+str(root/'main'),'-I'+str(root/'tools/hostshim'),*[str(root/s) for s in sources],str(cfile),'-lm','-o',str(binary)]
 if a.sanitize:cmd[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
 subprocess.run(cmd,check=True);subprocess.run([str(binary)],check=True)
