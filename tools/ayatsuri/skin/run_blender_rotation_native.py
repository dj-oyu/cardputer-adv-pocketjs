"""Fresh public Blender -> generated buffers -> actual QuickJS Scene/native."""
import argparse,hashlib,json,shutil,struct,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3];sys.path.insert(0,str(ROOT/'tools/kasane_contract'))
from run_meshbench_app import build_harness
p=argparse.ArgumentParser();p.add_argument('--output-dir',type=Path,required=True);a=p.parse_args();out=a.output_dir.resolve();out.mkdir(parents=True,exist_ok=False)
def run(args):return subprocess.check_output(list(map(str,args)),cwd=ROOT,text=True)
inputs=[ROOT/'tools/ayatsuri/skin'/n for n in ['blender_rotation_fixture.py','compile_blender_rotation_fixture.cjs','verify_blender_rotation_oracle.cjs','test_blender_rotation_native.c','blender_rotation_native_scenario.js','run_blender_rotation_native.py','test_js.c']]+[ROOT/'apps/lib/ayatsuri_model.js']
hashes=lambda ps:{str(x.relative_to(ROOT)):hashlib.sha256(x.read_bytes()).hexdigest() for x in ps}
start_head=run(['git','rev-parse','HEAD']).strip();start_tree=run(['git','rev-parse','HEAD^{tree}']).strip()
before=hashes(inputs)
print(run(['blender','--background','--factory-startup','--python',ROOT/'tools/ayatsuri/skin/blender_rotation_fixture.py','--',out/'source']))
run(['node',ROOT/'tools/ayatsuri/skin/compile_blender_rotation_fixture.cjs',out/'source/receipt.json',out/'compiled'])
report=json.loads(run(['node',ROOT/'tools/ayatsuri/skin/verify_blender_rotation_oracle.cjs',out/'source/receipt.json',out/'compiled/arrays.cjs']))
(out/'oracle.json').write_text(json.dumps(report,indent=2)+'\n')
arrays=(out/'compiled/arrays.cjs').read_text();assert arrays.startswith('module.exports=');(out/'arrays.js').write_text('globalThis.asset='+arrays[len('module.exports='):])
shutil.copyfile(out/'compiled/model.aynm',out/'skin-plain.aynm')
values=[n for s in report['predictions'] for v in s['vertices'] for n in v['position']+v['normal']];assert len(values)==108
(out/'expected.bin').write_bytes(struct.pack('<108f',*values))
generated=[out/'source/receipt.json',out/'compiled/arrays.cjs',out/'compiled/model.aynm',out/'arrays.js',out/'expected.bin',out/'skin-plain.aynm']
generated_hash=lambda:{str(x.relative_to(out)):hashlib.sha256(x.read_bytes()).hexdigest() for x in generated}
generated_before=generated_hash()
binary,build,env=build_harness('tools/ayatsuri/skin/test_blender_rotation_native.c','blender-rotation-native-host');env['ASAN_OPTIONS']='detect_leaks=0:abort_on_error=1'
for rows in (4,8):
 for direct in (0,1):
  r=subprocess.run([str(binary),str(rows),str(direct),str(out)],cwd=ROOT,env=env,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT);(out/f'rows{rows}-direct{direct}.log').write_text(r.stdout);print(r.stdout,end='');r.check_returncode()
assert hashes(inputs)==before
assert generated_hash()==generated_before
assert run(['git','rev-parse','HEAD']).strip()==start_head
artifacts=[x for x in out.rglob('*') if x.is_file()];(out/'report.json').write_text(json.dumps({'passed':True,'head':start_head,'tree':start_tree,'generatedInputsSha256':generated_before,'sourceSha256':before,'artifactSha256':{str(x.relative_to(out)):hashlib.sha256(x.read_bytes()).hexdigest() for x in artifacts},'sanitizers':'ASan/UBSan; LSan disabled','scope':'three generated poses including child X30 rotation; raw native scalar position/normal against independent oracle; actual QuickJS Scene renders vs shared native renderer reference; front/back culling and pending source disposal; no independent shading oracle/nonzero node TRS/device'},indent=2)+'\n')
