"""Compare row-band graph composition with the retained full-frame reference."""
import argparse, os, pathlib, subprocess
p=argparse.ArgumentParser();p.add_argument('--out',type=pathlib.Path,required=True);p.add_argument('--sanitize',action='store_true');a=p.parse_args()
root=pathlib.Path(__file__).resolve().parents[3];out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
c=out/'test.c';c.write_text(r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "tools/hone/native_geometry/alpha_graph_band_candidate.h"
#include "tools/hone/native_geometry/alpha_graph_clip_candidate.h"
static uint16_t pixels[240*135],expected[240*135],row[240];
static float full_depth[240*135],depth[240],scratch[16],other[16];
int main(void){
 int owner;const double drivers[]={.7};
 const hone_alpha_instruction ac[]={{.op=HONE_ALPHA_X},{.op=HONE_ALPHA_MUL,.a=0,.b=0}};
 const hone_alpha_instruction cc[]={{.op=HONE_ALPHA_DRIVER,.a=0}};
 const hone_alpha_program alpha={ac,2,1},color[3]={{cc,1,0},{cc,1,0},{cc,1,0}};
 hone_graph_triangle t={.v={{-20,-10,2,{0,0,0}},{250,30,2,{1,0,0}},{30,150,2,{.5,1,0}}},.alpha=&alpha,.color=color};
 hone_graph_triangle clipped[6];size_t count;assert(hone_graph_clip_viewport(&t,0,0,240,135,clipped,6,&count));
 hone_graph_packet packets[16];for(size_t i=0;i<count;i++)packets[i]=(hone_graph_packet){clipped[i],(uint32_t)i};
 hone_graph_band_payload payload={.graph={packets,count,16,drivers,1,&owner,1,1},.binding=&owner,.generation=1,.span_limit=240};
 hone_graph_band_work work={&owner,1,row,depth,scratch,16};
 ksn_mesh_snapshot opaque={.enabled=true};
 assert(hone_graph_band_preflight(&opaque,&payload,&work));
 for(unsigned i=0;i<240*135;i++)full_depth[i]=-INFINITY;
 assert(hone_graph_render(&payload.graph,&owner,1,expected,full_depth,240,135,8,240,other,16));
 for(unsigned stride=1;stride<=8;stride*=2){
  memset(pixels,0xa5,sizeof pixels);
  for(unsigned y=0;y<135;y+=stride){unsigned n=135-y<stride?135-y:stride;assert(hone_graph_band_render(&opaque,&payload,&work,y,n,pixels+y*240));}
  assert(!memcmp(pixels,expected,sizeof pixels));
 }
 hone_graph_band_work alias=work;alias.scratch=(void*)packets;
 assert(!hone_graph_band_preflight(&opaque,&payload,&alias));
 payload.generation=2;assert(!hone_graph_band_preflight(&opaque,&payload,&work));payload.generation=1;
 payload.span_limit=1;assert(!hone_graph_band_preflight(&opaque,&payload,&work));payload.span_limit=240;
 /* Five identical covered layers must fail before any output-band write. */
 for(unsigned i=0;i<5;i++)packets[i]=(hone_graph_packet){clipped[0],i};payload.graph.count=5;
 memset(pixels,0xa5,sizeof pixels);assert(!hone_graph_band_preflight(&opaque,&payload,&work));
 for(unsigned i=0;i<240*135;i++)assert(pixels[i]==0xa5a5);
 puts("GRAPH_BAND PASS full-reference pixel parity rows1/2/4/8 clipped_nonlinear_local alpha overflow/binding/alias");
}
''')
sources=['main/ayatsuri/compact_opaque.c','main/ui/kasane/ksn_shiki_frame.c','main/ui/kasane/ksn_mesh_depth.c']
for opt in ('0','2'):
 exe=out/('band'+opt+('.exe' if os.name=='nt' else ''))
 cmd=[os.getenv('CC','cc'),'-std=c11','-O'+opt,'-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-ffp-contract=off','-fno-fast-math','-I'+str(root),'-I'+str(root/'main'),'-I'+str(root/'tools/hostshim')]
 if a.sanitize:cmd+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
 subprocess.run(cmd+[str(root/s) for s in sources]+[str(c),'-lm','-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
