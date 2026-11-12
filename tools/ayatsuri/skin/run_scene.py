#!/usr/bin/env python3
"""Real QuickJS explicit Scene skin evaluation and lifecycle."""
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
p.add_argument('--no-leak-check', action='store_true', help='Keep ASan/UBSan; explicitly disable LSan for ptrace hosts')
a = p.parse_args()
inputs = ['apps/lib/ayatsuri_motion.js', 'tools/ayatsuri/skin/js_scenario.js',
          'tools/ayatsuri/skin/motion_scenario.js', 'tools/ayatsuri/skin/test_js.c',
          'tools/ayatsuri/skin/test_motion.c', 'tools/ayatsuri/skin/run_scene.py', 'apps/lib/ayatsuri_model.js', 'tools/ayatsuri/skin/test_scene_skin.c', 'tools/ayatsuri/skin/scene_scenario.js']
before = {p: hashlib.sha256((ROOT/p).read_bytes()).hexdigest() for p in inputs}
fixture_dir = ROOT/'build_skin_js'
subprocess.run(['node', str(Path(__file__).with_name('render_fixture.cjs')), str(fixture_dir)], check=True)
binary, out, env = build_harness('tools/ayatsuri/skin/test_scene_skin.c', 'scene-skin-host')
if a.no_leak_check:
    env['ASAN_OPTIONS'] = 'detect_leaks=0:abort_on_error=1'
for rows in [4, 8]:
    for direct in [0, 1]:
        result = subprocess.run([str(binary), str(rows), str(direct), str(fixture_dir)], cwd=ROOT,
                                env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (out/f'rows{rows}-direct{direct}.log').write_text(result.stdout)
        print(result.stdout, end='')
        result.check_returncode()
after = {p: hashlib.sha256((ROOT/p).read_bytes()).hexdigest() for p in inputs}
if before != after:
    raise RuntimeError('Motion test inputs changed while running')
(out/'scene-inputs.json').write_text(json.dumps({
    'head': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
    'asan_ubsan': True, 'lsan': not a.no_leak_check,
    'inputs_sha256': after, 'inputs_unchanged': True,
    'logs_sha256': {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(out.glob('rows*-direct*.log'))}
}, indent=2)+'\n')
print('SCENE_SKIN_ALL_ARMS_PASS actual QuickJS and renderer; no device acceptance')
