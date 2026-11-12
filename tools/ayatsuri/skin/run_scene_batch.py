#!/usr/bin/env python3
"""Real QuickJS bounded multi-mesh skin batch, renderer, OOM and lifetime."""
import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT/'tools/kasane_contract'))
from run_meshbench_app import build_harness

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--no-leak-check', action='store_true', help='Explicitly disable LSan on ptrace hosts; keep ASan/UBSan')
a = p.parse_args()
inputs = sorted(set([*ROOT.glob('main/ayatsuri/*.[ch]'), *ROOT.glob('main/pocket/*.inc'), ROOT/'apps/lib/ayatsuri_model.js', *Path(__file__).parent.glob('*batch*'), Path(__file__).parent/'test_js.c']))
before = {str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs if p.is_file()}
fixture = ROOT/'build_skin_batch' 
for name in ['render_fixture.cjs', 'batch_fixture.cjs']:
    subprocess.run(['node', str(Path(__file__).with_name(name)), str(fixture)], check=True)
binary, out, env = build_harness('tools/ayatsuri/skin/test_scene_batch.c', 'scene-batch-host')
if a.no_leak_check:
    env['ASAN_OPTIONS'] = 'detect_leaks=0:abort_on_error=1'
for rows in [4, 8]:
    for direct in [0, 1]:
        result = subprocess.run([str(binary), str(rows), str(direct), str(fixture)], cwd=ROOT,
                                env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (out/f'rows{rows}-direct{direct}.log').write_text(result.stdout)
        print(result.stdout, end='')
        result.check_returncode()
print('SKIN_BATCH_ALL_ARMS_PASS actual QuickJS and renderer; no device acceptance')

after = {str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs if p.is_file()}
if before != after: raise RuntimeError('inputs changed during run')
(out/'scene-batch-inputs.json').write_text(json.dumps({'head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),'inputs':after,'logs':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in out.glob('rows*-direct*.log')}},indent=2)+'\n')
