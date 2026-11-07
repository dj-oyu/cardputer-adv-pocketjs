#!/usr/bin/env python3
"""Opt-in native provider contract, only public generated synthetic geometry."""
import argparse,os,subprocess
from pathlib import Path
root=Path(__file__).resolve().parents[3]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--sanitize',action='store_true')
p.add_argument('--opt',choices=['0','2'],default='2')
p.add_argument('--build-dir',type=Path,default=root/'build_provider')
a=p.parse_args();out=a.build_dir.resolve();out.mkdir(parents=True,exist_ok=True)
subprocess.run(['node',str(Path(__file__).with_name('fixture.cjs')),str(out/'synthetic.aynm')],check=True)
subprocess.run(['node',str(root/'tools/ayatsuri/skin/render_fixture.cjs'),str(out)],check=True)
sources=['main/ayatsuri/ayatsuri_model.c','main/ayatsuri/ayatsuri_visibility.c','main/ayatsuri/compact_opaque.c','main/ui/kasane/ksn_shiki_frame.c','main/ui/kasane/ksn_mesh_depth.c','tools/ayatsuri/provider/test_provider.c']
command=[os.environ.get('CC','cc'),'-std=gnu11','-O'+a.opt,'-g','-Wall','-Wextra','-Werror','-ffp-contract=off','-fno-fast-math','-DAYATSURI_MODEL_EXPERIMENTAL_PROVIDER','-I'+str(root/'main'),'-I'+str(root/'tools/hostshim'),*[str(root/s) for s in sources],'-lm','-pthread','-Wl,--wrap=malloc,--wrap=calloc,--wrap=free','-o',str(out/'provider')]
if a.sanitize:command[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
(out/'command.txt').write_text(' '.join(command)+'\n')
subprocess.run(command,check=True)
env=dict(os.environ)
if a.sanitize:env['ASAN_OPTIONS']='detect_leaks=0:abort_on_error=1'
subprocess.run([str(out/'provider'),str(out/'synthetic.aynm')],check=True,env=env)
