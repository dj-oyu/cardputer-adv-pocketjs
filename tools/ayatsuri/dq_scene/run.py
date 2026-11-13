import hashlib,json,subprocess,sys
from pathlib import Path
R=Path(__file__).resolve().parents[3];D=Path(__file__).resolve().parent;sys.path.insert(0,str(R/'tools/kasane_contract'))
from run_meshbench_app import build_harness
out=R/'.cache/dq-scene-input';out.mkdir(parents=True,exist_ok=True)
subprocess.run([sys.executable,str(D/'oracle.py')],cwd=out,check=True)
cases=json.loads((out/'expected.json').read_text())['cases']
(out/'cases.js').write_text('globalThis.dqCases='+json.dumps(cases)+';')
subprocess.run(['node',str(D/'fixture.cjs'),str(out)],check=True)
files=[*D.glob('*.*'),R/'apps/lib/ayatsuri_model.js',R/'main/pocket/pocket_ayatsuri_model.inc',R/'main/ayatsuri/ayatsuri_skin_dq.h',R/'tools/ayatsuri/model/pack_model.cjs',R/'tools/ayatsuri/skin/test_motion.c',R/'tools/ayatsuri/skin/test_js.c',R/'tools/kasane_contract/run_meshbench_app.py']
h=lambda:{str(p.relative_to(R)):hashlib.sha256(p.read_bytes()).hexdigest() for p in files if p.is_file()}
before=h();binary,logs,env=build_harness('tools/ayatsuri/dq_scene/test_scene.c','dq-scene-host');env['ASAN_OPTIONS']='detect_leaks=0:abort_on_error=1'
for rows in [4,8]:
 for direct in [0,1]:
  p=subprocess.run([str(binary),str(rows),str(direct),str(out)],cwd=R,env=env,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT);(logs/f'rows{rows}-direct{direct}.log').write_text(p.stdout);print(p.stdout,end='');p.check_returncode()
assert before==h()
(logs/'inputs.json').write_text(json.dumps({'head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=R,text=True).strip(),'sources':h(),'generated':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in out.iterdir() if p.is_file()},'logs':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in logs.glob('rows*.log')}},indent=2))
