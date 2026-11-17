"""Shared normal workspace parity on complete synthetic meshes, not target timing."""
import argparse, os, pathlib, subprocess
from native_geometry import emit
from test_consumer_plan import fixture
p=argparse.ArgumentParser();p.add_argument('--out',type=pathlib.Path,required=True);p.add_argument('--opt',choices=['0','2'],default='2');p.add_argument('--sanitize',action='store_true');a=p.parse_args()
here=pathlib.Path(__file__).resolve().parent;out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
(out/'geometry.h').write_text(emit(fixture()))
(out/'test.c').write_text(r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "geometry.h"
#include "native_normals_batch.h"
#include "../source_package/native_wedge_lut_candidate.c"
__attribute__((noinline)) int run_batch(const hone_normal_job*j,size_t n,double*w,size_t count,hone_normal_kernel fn){return hone_normals_batch(j,n,w,count,fn);}
int main(void){
 const hone_input_mesh*m=hone_input_meshes;
 float positions[2][12],out[2][18],ref[2][18];double shared[12],separate[2][12];
 hone_normal_job jobs[2]={{m,positions[0],12,out[0],18},{m,positions[1],12,out[1],18}};
 for(unsigned frame=0;frame<100;frame++){
  for(unsigned i=0;i<2;i++){
   memcpy(positions[i],m->source_position,sizeof positions[i]);positions[i][11]+=(float)(frame+i)/100;
   assert(polygon_normals(positions[i],4,m->polygon_offsets,m->polygon_vertices,m->polygon_smooth,2,6,separate[i],ref[i]));
  }
  assert(run_batch(jobs,2,shared,12,polygon_normals));assert(!memcmp(out,ref,sizeof out));
 }
 /* Capacity failure on the last job must reject before either output changes. */
 memset(out,0x55,sizeof out);float saved[2][18];memcpy(saved,out,sizeof out);
 jobs[1].normal_values=17;assert(!run_batch(jobs,2,shared,12,polygon_normals));assert(!memcmp(out,saved,sizeof out));jobs[1].normal_values=18;
 assert(!run_batch(jobs,2,shared,11,polygon_normals));assert(!memcmp(out,saved,sizeof out));
 /* A kernel failure may retain earlier unpublished mesh output. */
 positions[1][0]=NAN;assert(!run_batch(jobs,2,shared,12,polygon_normals));assert(!memcmp(out[0],ref[0],sizeof ref[0]));assert(!memcmp(out[1],saved[1],sizeof saved[1]));
 positions[1][0]=0;assert(run_batch(jobs,2,shared,12,polygon_normals));
 printf("PASS normal batch 100 frames; parity; late capacity/kernel failure; retry; work bytes %zu -> %zu\n",sizeof separate,sizeof shared);
 return 0;
}
''')
binary=out/('test.exe' if os.name=='nt' else 'test')
cmd=[os.environ.get('CC','cc'),'-std=c11','-O'+a.opt,'-g','-Wall','-Wextra','-Werror','-fstack-usage','-ffp-contract=off','-fno-fast-math','-I'+str(here),str(out/'test.c'),'-lm','-o',str(binary)]
if a.sanitize:cmd[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
subprocess.run(cmd,check=True);subprocess.run([str(binary)],check=True)
for f in out.glob('*.su'):
 for line in f.read_text().splitlines():
  if any(n in line for n in ('run_batch','hone_normals_batch','polygon_normals')):print(line)
