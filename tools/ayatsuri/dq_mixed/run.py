import hashlib,json,subprocess,sys
from pathlib import Path
R=Path(__file__).resolve().parents[3];D=Path(__file__).resolve().parent;sys.path.insert(0,str(R/'tools/kasane_contract'))
import run_meshbench_app as host
from types import SimpleNamespace
real_run=subprocess.run
actual=[]
def wrapped_run(cmd,**kw):
 cmd=list(map(str,cmd))
 if cmd[0]=='cc' and any(x.endswith('dq_mixed/test_scene.c') for x in cmd):
  cmd.append('-Wl,--wrap=ayatsuri_model_instance_evaluate_meshes');actual.append(cmd)
 return real_run(cmd,**kw)
host.subprocess=SimpleNamespace(run=wrapped_run,check_output=subprocess.check_output,DEVNULL=subprocess.DEVNULL)
build_harness=host.build_harness
out=R/'.cache/dq-mixed-input';out.mkdir(parents=True,exist_ok=True)
subprocess.run([sys.executable,str(D/'oracle.py')],cwd=out,check=True)
cases=json.loads((out/'expected.json').read_text())['cases']
import struct
(out/'expected.bin').write_bytes(struct.pack('<'+str(len(cases)*24)+'f',*[v for c in cases for v in c['expected']]))
(out/'cases.js').write_text('globalThis.dqCases='+json.dumps(cases)+';')
subprocess.run(['node',str(D/'fixture.cjs'),str(out)],check=True)
files=[*D.glob('*.*'),R/'apps/lib/ayatsuri_model.js',R/'main/pocket/pocket_ayatsuri_model.inc',R/'main/ayatsuri/ayatsuri_skin_dq.h',R/'tools/ayatsuri/model/pack_model.cjs',R/'tools/ayatsuri/skin/test_motion.c',R/'tools/ayatsuri/skin/test_js.c',R/'tools/kasane_contract/run_meshbench_app.py']
h=lambda:{str(p.relative_to(R)):hashlib.sha256(p.read_bytes()).hexdigest() for p in files if p.is_file()}
before=h();binary,logs,env=build_harness('tools/ayatsuri/dq_mixed/test_scene.c','dq-mixed-host');ev=json.loads((logs/'commands.json').read_text());ev['commands'][-1]=actual[-1];(logs/'commands.json').write_text(json.dumps(ev,indent=2));env['ASAN_OPTIONS']='detect_leaks=0:abort_on_error=1'
for rows in [4,8]:
 for direct in [0,1]:
  p=subprocess.run([str(binary),str(rows),str(direct),str(out)],cwd=R,env=env,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT);(logs/f'rows{rows}-direct{direct}.log').write_text(p.stdout);print(p.stdout,end='');p.check_returncode()
assert before==h()
(logs/'inputs.json').write_text(json.dumps({'head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=R,text=True).strip(),'sources':h(),'generated':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in out.iterdir() if p.is_file()},'logs':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in logs.glob('rows*.log')}},indent=2))
