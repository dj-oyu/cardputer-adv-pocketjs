"""Independent reproduction of sol's photo finishing-order candidate on actual DERBY.
One common O2 native binary; only lowered app input differs between A and B.
"""
from pathlib import Path
import os,sys,subprocess,hashlib,json,datetime,argparse,shutil
R=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(R/'tools/games'))
import run_derby
BASE='e0fe98a8c2a3de3b48c8d547c1ff3c8d99863267'

def prepare(out):
 changed=set(subprocess.check_output(['git','diff','--name-only',BASE,'--','apps/derby'],cwd=R,text=True).splitlines());assert changed <= {'apps/derby/derby_scene.js'},changed
 out.mkdir(parents=True,exist_ok=True)
 baseline=out/'baseline-source'
 if baseline.exists():shutil.rmtree(baseline)
 shutil.copytree(R/'apps/derby',baseline)
 (baseline/'derby_scene.js').write_bytes(subprocess.check_output(['git','show',BASE+':apps/derby/derby_scene.js'],cwd=R))
 return {label:run_derby.lower(src,out/label/'apps/derby') for label,src in [('baseline',baseline),('candidate',R/'apps/derby')]}

def manifest(apps):
 return {mode:{str(p.relative_to(app)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(app.rglob('*')) if p.is_file()} for mode,app in apps.items()}
