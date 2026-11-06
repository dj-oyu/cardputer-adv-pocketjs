"""Opt-in Q7 product lifecycle test; original default runner remains unchanged."""
import os,runpy,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
out=ROOT/'.cache/q7-lifecycle';out.mkdir(parents=True,exist_ok=True)
(out/'original_q7_helpers.inc').write_text((ROOT/'tools/kasane_contract/test_mesh_depth_qjs.c').read_text().replace('int main(void){','int q7_inherited_depth_suite(void){'))
original=subprocess.run
sys.argv=[sys.argv[0]]
def run(cmd,*args,**kwargs):
 cmd=list(map(str,cmd))
 if cmd and cmd[0]=='cc':
  cmd+=['-I'+str(out),'-DKSN_MESH_PREPARED_PROTOTYPE=1','-DKSN_MESH_Q7_CERTIFIED_PROTOTYPE=1','-fno-fast-math','-fno-associative-math','-fno-reciprocal-math','-ffp-contract=off','-frounding-math']
  if any(x.endswith('/pocket_shiki_frame.c') for x in cmd):
   cmd+=[str(ROOT/'main/ui/kasane/ksn_mesh_prepared.c'),str(ROOT/'main/ui/kasane/ksn_mesh_q7_certified.c')]
  cmd=[x.replace('/test_mesh_depth_qjs.c','/test_mesh_q7_lifecycle.c') for x in cmd]
 if 'env' in kwargs:kwargs['env']=dict(kwargs['env'],ASAN_OPTIONS='detect_leaks=0:abort_on_error=1')
 return original(cmd,*args,**kwargs)
subprocess.run=run
runpy.run_path(str(ROOT/'tools/kasane_contract/run_mesh_depth.py'),run_name='__main__')
