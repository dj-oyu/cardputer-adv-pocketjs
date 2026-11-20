"""Portable packed admission/lifetime and direct pixel reference tests."""
import argparse,os,pathlib,subprocess
p=argparse.ArgumentParser();p.add_argument('--out',type=pathlib.Path,required=True);p.add_argument('--sanitize',action='store_true');a=p.parse_args()
root=pathlib.Path(__file__).resolve().parents[3];out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
(out/'test.c').write_text(r'''
#include "tools/hone/native_geometry/projected_sample_candidate.h"
#include "tools/hone/native_geometry/alpha_graph_occlusion_candidate.h"
#include <assert.h>
#include <stdio.h>
int main(void){
 hone_alpha_instruction code[4]={{.op=HONE_ALPHA_CONST,.value=1},{.op=HONE_ALPHA_CONST,.value=.2f},{.op=HONE_ALPHA_CONST,.value=.4f},{.op=HONE_ALPHA_CONST,.value=.8f}};
 hone_alpha_program alpha={code,1,0},color[3]={{code+1,1,0},{code+2,1,0},{code+3,1,0}};
 hone_projected_material materials[1];assert(hone_projected_material_admit(&alpha,color,materials));hone_projected_material before=materials[0];
 code[0].value=nextafterf(1,0);assert(!hone_projected_material_admit(&alpha,color,materials)&&!memcmp(materials,&before,sizeof before));code[0].value=1;
 int owner;hone_projected_packet packets[64];hone_projected_snapshot s={0};assert(hone_projected_snapshot_init(&s,&owner,1,packets,64,materials,1));assert(hone_projected_begin(&s,&owner,1,1));
 hone_graph_packet reference[64];size_t count=0;
 for(unsigned j=0;j<6;j++){
  float y=(float)(j*24);hone_graph_triangle t={.v={{-10,y,2,{.1,.2,.3}},{245,y,2,{.2,.3,.4}},{120,y+28,3,{.3,.4,.5}}},.alpha=&alpha,.color=color};
  hone_graph_triangle clipped[6];size_t n=0;assert(hone_graph_clip_viewport(&t,0,0,240,135,clipped,6,&n));
  unsigned order=j%3;assert(hone_projected_append(&s,&t,0,order));for(size_t k=0;k<n;k++)reference[count++]=(hone_graph_packet){clipped[k],order+(uint32_t)k};
 }
 assert(s.count==count&&hone_projected_seal(&s));hone_graph_snapshot graph={.packets=reference,.count=count,.capacity=64,.owner=&owner,.generation=1,.ready=1};float scratch[1];
 for(size_t j=0;j<count;j++)for(unsigned k=0;k<3;k++){
  const hone_projected_packet*p=hone_projected_at(&s,j);assert(p&&p->xyz[3*k]==reference[j].triangle.v[k].x&&p->xyz[3*k+1]==reference[j].triangle.v[k].y&&p->xyz[3*k+2]==reference[j].triangle.v[k].z&&p->order==reference[j].order);
 }
 for(unsigned y=0;y<135;y++)for(unsigned x=0;x<240;x++){
  hone_graph_ordered f[4];int n=hone_graph_fragments_occlusion(&graph,x,y,-INFINITY,scratch,1,f);assert(n>=0);uint16_t expected=0,pixel=0;
  if(n){float*r=f[n-1].fragment.rgb;expected=(uint16_t)(((unsigned)floorf(r[0]*31+.5f)<<11)|((unsigned)floorf(r[1]*63+.5f)<<5)|(unsigned)floorf(r[2]*31+.5f));}
  int hit=hone_projected_pixel(&s,x,y,-INFINITY,&pixel);assert(hit>=0&&pixel==expected);
 }
 assert(hone_projected_pin(&s,&owner,1,1));assert(!hone_projected_begin(&s,&owner,1,2));assert(!hone_projected_finish(&s,1,false)&&s.pinned);assert(!hone_projected_finish(&s,2,true));assert(hone_projected_finish(&s,1,true));
 assert(hone_projected_begin(&s,&owner,1,2));hone_graph_triangle t=reference[0].triangle;t.v[0].local[0]=NAN;assert(!hone_projected_append(&s,&t,0,0)&&!s.count&&!hone_projected_seal(&s));
 assert(hone_projected_begin(&s,&owner,1,3));t=reference[0].triangle;assert(hone_projected_append(&s,&t,0,0));hone_projected_packet saved=packets[0];size_t old=s.count;s.capacity=old;assert(!hone_projected_append(&s,&t,0,1)&&s.count==old&&!memcmp(&saved,packets,sizeof saved)&&!hone_projected_seal(&s));s.capacity=64;
 assert(hone_projected_begin(&s,&owner,1,4));t=(hone_graph_triangle){.v={{-10,0,1,{0,0,0}},{20,0,1,{0,0,0}},{20,20,1,{0,0,0}}},.alpha=&alpha,.color=color};assert(!hone_projected_append(&s,&t,0,UINT32_MAX)&&!s.count);
 assert(hone_projected_begin(&s,&owner,1,5));t=reference[0].triangle;t.v[0].local[0]=FLT_MAX;assert(!hone_projected_append(&s,&t,0,0)&&!s.count&&!hone_projected_seal(&s));
 puts("PROJECTED PASS packet44/material16 exact_geometry/pixels clipping/order admission/failure_rollback/pin_drain");
}
''')
for opt in ('0','2'):
 exe=out/('test'+opt+('.exe' if os.name=='nt' else ''))
 cmd=[os.getenv('CC','cc'),'-std=c11','-O'+opt,'-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-ffp-contract=off','-fno-fast-math','-I'+str(root),'-I'+str(root/'main')]
 if a.sanitize:cmd+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
 subprocess.run(cmd+[str(out/'test.c'),'-lm','-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
