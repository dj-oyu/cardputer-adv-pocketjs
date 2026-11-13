import hashlib,json,subprocess,sys
from pathlib import Path
R=Path(__file__).resolve().parents[2];sys.path.insert(0,str(R/'tools/kasane_contract'))
from run_meshbench_app import build_harness
inputs=[*Path(__file__).parent.glob('*.*'),R/'apps/lib/ayatsuri_model.js',R/'tools/ayatsuri/skin/render_fixture.cjs']
h=lambda:{str(p.relative_to(R)):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs if p.is_file()}
before=h();fixture=R/'.cache/blink-visibility-fixture'
subprocess.run(['node',str(R/'tools/ayatsuri/skin/render_fixture.cjs'),str(fixture)],check=True)
binary,out,env=build_harness('tools/blink_visibility/test_visibility.c','blink-visibility-host');env['ASAN_OPTIONS']='detect_leaks=0:abort_on_error=1'
for rows in [4,8]:
 for direct in [0,1]:
  r=subprocess.run([str(binary),str(rows),str(direct),str(fixture)],cwd=R,env=env,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT);(out/f'rows{rows}-direct{direct}.log').write_text(r.stdout);print(r.stdout,end='');r.check_returncode()
assert before==h()
(out/'inputs.json').write_text(json.dumps({'head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=R,text=True).strip(),'inputs':h(),'logs':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in out.glob('rows*-direct*.log')}},indent=2)+'\n')
