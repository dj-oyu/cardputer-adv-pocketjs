"""Compile the bounded gate against production packet/retained/Q7 sources."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
parser = argparse.ArgumentParser()
parser.add_argument('--plain', action='store_true')
args = parser.parse_args()
flags = ['-std=c11', '-O2', '-g', '-fno-fast-math', '-fno-associative-math',
         '-fno-reciprocal-math', '-ffp-contract=off', '-frounding-math',
         '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'main')]
if not args.plain:
    flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
sources = ['main/ayatsuri/compact_opaque.c', 'main/ui/kasane/ksn_mesh_depth.c',
           'main/ui/kasane/ksn_shiki_frame.c', 'main/ui/kasane/ksn_mesh_prepared.c',
           'main/ui/kasane/ksn_mesh_q7_certified.c',
           'main/ui/kasane/ksn_mesh_q7_preflight.c',
           'tools/kasane_contract/q7/test_preflight.c']
with tempfile.TemporaryDirectory(prefix='q7-preflight-') as directory:
    binary = Path(directory) / 'preflight'
    subprocess.run([os.environ.get('CC', 'cc'), *flags,
                    *[str(ROOT / p) for p in sources], '-Wl,--wrap=ksn_mesh_q7_render', '-lm', '-o', str(binary)], check=True)
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',
               UBSAN_OPTIONS='halt_on_error=1')
    subprocess.run([str(binary)], check=True, env=env)
    workspace = Path(directory) / 'workspace'
    subprocess.run([os.environ.get('CC', 'cc'), *flags,
                    '-I'+str(ROOT/'main/ui/kasane'),
                    str(ROOT/'tools/kasane_contract/q7/test_workspace.c'),
                    '-o', str(workspace)], check=True)
    subprocess.run([str(workspace)], check=True, env=env)
