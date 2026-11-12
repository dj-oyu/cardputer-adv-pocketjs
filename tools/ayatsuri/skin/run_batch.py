#!/usr/bin/env python3
"""Real QuickJS bounded multi-mesh skin batch, renderer, OOM and lifetime."""
import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT/'tools/kasane_contract'))
from run_meshbench_app import build_harness

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--no-leak-check', action='store_true', help='Explicitly disable LSan on ptrace hosts; keep ASan/UBSan')
a = p.parse_args()
fixture = ROOT/'build_skin_batch'
for name in ['render_fixture.cjs', 'batch_fixture.cjs']:
    subprocess.run(['node', str(Path(__file__).with_name(name)), str(fixture)], check=True)
binary, out, env = build_harness('tools/ayatsuri/skin/test_batch.c', 'skin-batch-host')
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
