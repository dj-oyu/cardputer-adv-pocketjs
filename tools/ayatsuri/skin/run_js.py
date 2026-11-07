#!/usr/bin/env python3
"""Real QuickJS skin binding, frame submission, compositor, transport and GC."""
import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT/'tools/kasane_contract'))
from run_meshbench_app import build_harness

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--no-leak-check', action='store_true', help='Keep ASan/UBSan; explicitly disable LSan for ptrace hosts')
a = p.parse_args()
fixture_dir = ROOT/'build_skin_js'
subprocess.run(['node', str(Path(__file__).with_name('render_fixture.cjs')), str(fixture_dir)], check=True)
binary, out, env = build_harness('tools/ayatsuri/skin/test_js.c', 'skin-js-host')
if a.no_leak_check:
    env['ASAN_OPTIONS'] = 'detect_leaks=0:abort_on_error=1'
for rows in [4, 8]:
    for direct in [0, 1]:
        result = subprocess.run([str(binary), str(rows), str(direct), str(fixture_dir)], cwd=ROOT,
                                env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (out/f'rows{rows}-direct{direct}.log').write_text(result.stdout)
        print(result.stdout, end='')
        result.check_returncode()
print('SKIN_JS_ALL_ARMS_PASS actual QuickJS and renderer; no device acceptance')
