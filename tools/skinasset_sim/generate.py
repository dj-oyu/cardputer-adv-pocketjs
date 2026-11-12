"""Fixed public fixture bootstrap; isolated test instrumentation only."""
import hashlib,json,subprocess,sys
from pathlib import Path
root=Path(__file__).resolve().parents[2];source=Path(sys.argv[1]).resolve();out=Path(sys.argv[2]).resolve()
subprocess.run([sys.executable,str(root/'apps/skinasset/prepare.py'),str(source),str(out)],check=True)
p=out/'skin_asset_bootstrap.js';text=p.read_text();needle='globalThis.skinAssetStop=()=>sample.stop();'
assert text.count(needle)==1
# Expose existing stats through test-only closure; app/library source unchanged.
p.write_text(text.replace(needle,needle+'globalThis.skinAssetStats=out=>sample.stats(out);'))
files=[source/'source/receipt.json',source/'compiled/model.aynm',source/'compiled/arrays.cjs',p]
(out/'input-hashes.json').write_text(json.dumps({str(x):hashlib.sha256(x.read_bytes()).hexdigest() for x in files},indent=2)+'\n')
