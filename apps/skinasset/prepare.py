#!/usr/bin/env python3
"""Wrap only the fixed public receipt into an opt-in diagnostic bootstrap."""
import hashlib,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
source=Path(sys.argv[1]).resolve();out=Path(sys.argv[2]).resolve()
expected={'source/receipt.json':'03201a3999d15681b51ee58f28e506179d4504c61930b4bd90e081c96c9abf4f','compiled/model.aynm':'efadad8c8dcc863da48917763f4c51f6d62e3527fe355153bab15b30b3531948','compiled/arrays.cjs':'a6547160d5d59a3ab51ecc199f36aac3c2098a9f067a465883f37bdff1a3524f'}
for name,sha in expected.items():
 if hashlib.sha256((source/name).read_bytes()).hexdigest()!=sha:raise ValueError('not the fixed public fixture: '+name)
out.mkdir(parents=True,exist_ok=True)
arrays=(source/'compiled/arrays.cjs').read_text();assert arrays.startswith('module.exports=')
entry=(ROOT/'apps/skinasset/skin_asset_entry.js').read_text()
entry+='\n(()=>{const data='+arrays[len('module.exports='):].rstrip().removesuffix(';')+';\n'
entry+='const P=pocket.kasane.shiki,r=P.resource();pocket.kasane.replace(tx=>{tx.background(0x000011ff);tx.image({resource:r,bounds:[0,0,240,135],clip:[0,0,240,135],sourceWidth:240,sourceHeight:135});});\n'
entry+='const sample=startGeneratedSkinSample(new Uint8Array(['+','.join(map(str,(source/'compiled/model.aynm').read_bytes()))+']).buffer,data);\n'
entry+='globalThis.skinAssetFrame=timeMs=>sample.frame(timeMs);globalThis.skinAssetStop=()=>sample.stop();})();\n'
(out/'skin_asset_bootstrap.js').write_text(entry)
subprocess.run([sys.executable,str(ROOT/'tools/make_app_chunks.py'),str(out),str(ROOT/'apps/skinasset/optional_chunks.txt')],check=True)
print(out/'skin_asset_bootstrap.js')
