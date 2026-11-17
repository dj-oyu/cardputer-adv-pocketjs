"""Public synthetic differential frame candidate test; no target performance claim."""
import argparse, os, pathlib, subprocess
p=argparse.ArgumentParser();p.add_argument('--out',type=pathlib.Path,required=True);p.add_argument('--opt',choices=['0','2'],default='2');p.add_argument('--sanitize',action='store_true');a=p.parse_args()
here=pathlib.Path(__file__).resolve().parent;root=here.parents[2];out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
binary=out/('test.exe' if os.name=='nt' else 'test')
sources=['main/ayatsuri/compact_opaque.c','main/ui/kasane/ksn_shiki_frame.c','main/ui/kasane/ksn_mesh_depth.c']
cmd=[os.environ.get('CC','cc'),'-std=c11','-O'+a.opt,'-g','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-ffp-contract=off','-fno-fast-math','-I'+str(root/'main'),'-I'+str(root/'tools/hostshim'),*[str(root/s) for s in sources],str(here/'test_alpha_bounds_frame.c'),'-lm','-o',str(binary)]
if a.sanitize:cmd[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
subprocess.run(cmd,check=True);subprocess.run([str(binary)],check=True)
