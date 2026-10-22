#!/usr/bin/env python3
"""Exact diagnostic workload in the real host app, plus owner deadline boundaries."""
import argparse,ast,hashlib,json,os,shutil,subprocess,tempfile
from pathlib import Path
import run_megademo_bounds as bench
import prepare_megademo_bounds_diag as diag
p=argparse.ArgumentParser();p.add_argument('--common',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
# A capped log must still reject an automatic tier downgrade.
guard_test="const assert=require('node:assert/strict');let calls=0;globalThis.console={log(){++calls}};"+diag.PRELUDE+"for(let i=0;i<20;i++)console.log('ordinary');assert.equal(calls,16);assert.throws(()=>console.log('MEGADEMO DEGRADE tier=1'),/MEGA_DIAG_DEGRADE/);"
subprocess.run(['node','-e',guard_test],check=True)
# Reuse only the already-reviewed six-case owner boundary test, no import side effects.
test=a.common.parent/'test_derby_app_diag.py'
node=next(n for n in ast.parse(test.read_text()).body if isinstance(n,ast.FunctionDef) and n.name=='test_owner')
env={'subprocess':subprocess,'tempfile':tempfile,'Path':Path}
exec(compile(ast.Module(body=[node],type_ignores=[]),str(test),'exec'),env)
env['test_owner'](diag.owner_template(a.common.parent/'device_diag',bench.BASE))
sources=bench.source_variants(out);binary=bench.build(out);panels=[]
for mode in ['base','fast']:
 for wrapped in [False,True]:
  name=mode+('-diagnostic' if wrapped else '-ordinary');source=out/(name+'.js');source.write_text(sources[mode]+(diag.WORKLOAD if wrapped else ''))
  e=dict(os.environ,MEGA_FRAMES='168',MEGA_LOG='1',BOUNDS_DIAG='120' if wrapped else '0',BOUNDS_CSV=str(out/(name+'.csv')),BOUNDS_CAPTURE=str(out/(name+'.panels')))
  r=subprocess.run([str(binary),str(source)],cwd=bench.ROOT,env=e,text=True,capture_output=True)
  (out/(name+'.log')).write_text(r.stdout+r.stderr)
  assert r.returncode==0 and 'MEGADEMO_HOST PASS' in r.stdout,(name,r.stdout,r.stderr)
  panels.append((out/(name+'.panels')).read_bytes())
assert all(x==panels[0] for x in panels)
report={'base':bench.BASE,'callbacks':168,'warm':48,'measured':120,'last_sample':119,'panels':169,'sha256':hashlib.sha256(panels[0]).hexdigest(),'ordinary_and_diagnostic_equal':True,'owner_boundary_cases':6,'degrade_after_log_cap_rejected':True}
(out/'result.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report))
