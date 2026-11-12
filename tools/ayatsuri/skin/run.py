#!/usr/bin/env python3
"""Compile the detached scalar skin contract; no SDK, assets or device needed."""
import argparse
import os
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[3]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--sanitize', action='store_true')
parser.add_argument('--build-dir', type=Path, default=root / 'build_skin_synthetic')
args = parser.parse_args()
args.build_dir.mkdir(parents=True, exist_ok=True)
binary = args.build_dir.resolve() / ('skin_sanitized' if args.sanitize else 'skin')
command = [os.environ.get('CC', 'cc'), '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
           '-pedantic', '-ffp-contract=off', '-fno-fast-math',
           '-I', str(root / 'main/ayatsuri'), str(Path(__file__).with_name('test_skin.c')),
           '-lm', '-o', str(binary)]
if args.sanitize:
    command[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
subprocess.run(command, check=True)
subprocess.run([str(binary)], check=True)
