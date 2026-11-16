"""Run batch-wrap synthetic geometry through the real candidate/snapshot caller.
Requires repository checkout; does not modify production source or admit assets.
"""
import argparse,pathlib,subprocess,os
p=argparse.ArgumentParser();p.add_argument('--root',type=pathlib.Path,required=True);p.add_argument('--out',type=pathlib.Path,required=True);p.add_argument('--sanitize',action='store_true');a=p.parse_args();root=a.root.resolve();out=a.out.resolve();out.mkdir(parents=True,exist_ok=True);here=pathlib.Path(__file__).parent.resolve()
source=(root/'tools/ayatsuri/provider/test_provider.c').read_text()
anchor='\nstruct Provider {unsigned frame,calls;'
assert source.count(anchor)==1
# Preserve independent original geometry, then compare wrapped candidate to it.
source=source.replace('static void geometry(', 'static void original_geometry(',1)
adapter='''
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#include "nearest_batch_candidate.c"
#pragma GCC diagnostic pop
static void geometry(unsigned frame,unsigned node,ayatsuri_skin_result*out){
 original_geometry(frame,node,out);
 double points[96],result[96];uint32_t faces[32],triangles[3]={0,1,2};nearest_box boxes[1];size_t tests;
 double z=out[0].position[2],target[9]={-100,-100,z,100,-100,z,0,100,z};
 for(unsigned i=0;i<32;i++){points[3*i]=out[i].position[0];points[3*i+1]=out[i].position[1];points[3*i+2]=z+1;}
 assert(nearest_batch(points,32,target,3,triangles,1,boxes,1,result,faces,&tests));
 for(unsigned i=0;i<32;i++)for(unsigned k=0;k<3;k++){
  assert(fabs(result[3*i+k]-out[i].position[k])<1e-10);
  out[i].position[k]=(float)result[3*i+k];
 }
}
'''
source=source.replace(anchor,'\n'+adapter+anchor)
(out/'test_provider.c').write_text(source)
subprocess.run(['node',str(root/'tools/ayatsuri/provider/fixture.cjs'),str(out/'synthetic.aynm')],check=True)
subprocess.run(['node',str(root/'tools/ayatsuri/skin/render_fixture.cjs'),str(out)],check=True)
sources=['main/ayatsuri/ayatsuri_model.c','main/ayatsuri/ayatsuri_visibility.c','main/ayatsuri/compact_opaque.c','main/ui/kasane/ksn_shiki_frame.c','main/ui/kasane/ksn_mesh_depth.c']
cmd=['cc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-ffp-contract=off','-fno-fast-math','-DAYATSURI_MODEL_EXPERIMENTAL_PROVIDER','-I'+str(root/'main'),'-I'+str(root/'tools/hostshim'),'-I'+str(here),*[str(root/s) for s in sources],str(out/'test_provider.c'),'-lm','-pthread','-Wl,--wrap=malloc,--wrap=calloc,--wrap=free','-o',str(out/'provider')]
if a.sanitize:cmd[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
subprocess.run(cmd,check=True);env=dict(os.environ);env['ASAN_OPTIONS']='detect_leaks=0:abort_on_error=1'
subprocess.run([str(out/'provider'),str(out/'synthetic.aynm')],check=True,env=env)
