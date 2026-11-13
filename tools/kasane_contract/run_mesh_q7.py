"""Synthetic exact-Q7 certification tests. No device or private model inputs."""
import argparse,os,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
a=argparse.ArgumentParser();a.add_argument('--plain',action='store_true');args=a.parse_args()
flags=['-std=gnu11','-O2','-g','-fno-fast-math','-fno-associative-math','-fno-reciprocal-math','-ffp-contract=off','-frounding-math','-Wall','-Wextra','-Werror','-I'+str(ROOT/'main')]
if not args.plain:flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
shared=['main/ayatsuri/compact_opaque.c','main/ui/kasane/ksn_mesh_depth.c','main/ui/kasane/ksn_shiki_frame.c','main/ui/kasane/ksn_mesh_q7_certified.c','tools/kasane_contract/q7/oracle_prepared.c','tools/kasane_contract/q7/baseline_q7.c']
env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1')
with tempfile.TemporaryDirectory(prefix='q7-host-') as directory:
 for test in ['test_numeric','test_certificate']:
  binary=Path(directory)/test
  subprocess.run(['cc',*flags,*[str(ROOT/p) for p in shared],str(ROOT/'tools/kasane_contract/q7'/(test+'.c')),'-lm','-o',str(binary)],check=True)
  subprocess.run([str(binary)],check=True,env=env)
