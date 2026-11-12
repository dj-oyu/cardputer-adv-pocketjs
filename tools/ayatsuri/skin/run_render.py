#!/usr/bin/env python3
"""C skin -> immutable Model evaluation -> existing opaque snapshot rasterizer."""
import argparse
import os
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[3]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--sanitize', action='store_true')
p.add_argument('--opt', choices=['0', '2'], default='2')
p.add_argument('--build-dir', type=Path, default=root / 'build_skin_render')
a = p.parse_args()
out = a.build_dir.resolve()
out.mkdir(parents=True, exist_ok=True)
subprocess.run(['node', str(Path(__file__).with_name('render_fixture.cjs')), str(out)], check=True)
sources = ['main/ayatsuri/ayatsuri_model.c', 'main/ayatsuri/ayatsuri_visibility.c',
           'main/ayatsuri/compact_opaque.c', 'main/ui/kasane/ksn_shiki_frame.c',
           'main/ui/kasane/ksn_mesh_depth.c', 'tools/ayatsuri/skin/test_render.c']
command = [os.environ.get('CC', 'cc'), '-std=gnu11', '-O'+a.opt, '-g',
           '-Wall', '-Wextra', '-Werror', '-ffp-contract=off', '-fno-fast-math',
           '-I'+str(root/'main'), '-I'+str(root/'tools/hostshim'),
           *[str(root/s) for s in sources], '-lm',
           '-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free', '-o', str(out/'skin_render')]
if a.sanitize:
    command[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
subprocess.run(command, check=True)
subprocess.run([str(out/'skin_render'), str(out)], check=True)
