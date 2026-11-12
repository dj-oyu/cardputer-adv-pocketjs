"""Regenerate only repository-owned public two-part synthetic fixture."""
import hashlib,json,subprocess,sys
from pathlib import Path
root=Path(__file__).resolve().parents[2];out=Path(sys.argv[1]).resolve();out.mkdir(parents=True,exist_ok=True)
subprocess.run(['node',str(root/'tools/ayatsuri/skin/render_fixture.cjs'),str(out/'fixture')],check=True)
model=out/'fixture/skin-plain.aynm'
if hashlib.sha256(model.read_bytes()).hexdigest()!='24e40884befed04f141904a96faf20c947b2eefd358bb308929c9071aa856132':
 raise ValueError('public two-part fixture changed')
source=Path(__file__).with_name('bootstrap.js')
s=source.read_text();assert s.count('__MODEL_BYTES__')==1
p=out/'skin_asset_bootstrap.js';p.write_text(s.replace('__MODEL_BYTES__',json.dumps(list(model.read_bytes()))))
manifest=out/'optional_chunks.txt';manifest.write_text('app local.scenebatch\nscene '+str(root/'apps/lib/ayatsuri_model.js')+'\n')
subprocess.run([sys.executable,str(root/'tools/make_app_chunks.py'),str(out),str(manifest)],check=True)
files=[model,source,p,root/'apps/lib/ayatsuri_model.js',root/'tools/ayatsuri/skin/render_fixture.cjs']
(out/'input-hashes.json').write_text(json.dumps({str(x):hashlib.sha256(x.read_bytes()).hexdigest() for x in files},indent=2)+'\n')
