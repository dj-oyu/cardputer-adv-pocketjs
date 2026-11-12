#!/usr/bin/env python3
"""Generate an opt-in bootstrap and chunk table; never modifies firmware defaults."""
import subprocess
import sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
out=Path(sys.argv[1]).resolve()
out.mkdir(parents=True,exist_ok=True)
subprocess.run(['node',str(ROOT/'tools/ayatsuri/skin/render_fixture.cjs'),str(out)],check=True)
blob=(out/'skin-plain.aynm').read_bytes()
entry=(ROOT/'apps/skinsample/skin_entry.js').read_text()
entry+='\n(()=>{\nconst P=pocket.kasane.shiki,r=P.resource();\npocket.kasane.replace(tx=>{tx.background(0x000011ff);tx.image({resource:r,bounds:[0,0,240,135],clip:[0,0,240,135],sourceWidth:240,sourceHeight:135});});\n'
entry+='const sample=startSkinSample(new Uint8Array(['+','.join(map(str,blob))+']).buffer);\n'
entry+='globalThis.skinSampleFrame=timeMs=>sample.frame(timeMs);\nglobalThis.skinSampleStop=()=>sample.stop();\n})();\n'
(out/'skin_bootstrap.js').write_text(entry)
subprocess.run([sys.executable,str(ROOT/'tools/make_app_chunks.py'),str(out),str(ROOT/'apps/skinsample/optional_chunks.txt')],check=True)
print('OPTIONAL_BOOTSTRAP',out/'skin_bootstrap.js')
