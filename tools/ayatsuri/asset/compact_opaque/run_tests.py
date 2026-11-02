#!/usr/bin/env python3
"""Synthetic-only, dependency-free host checks. No assets, network or firmware."""
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parent
subprocess.run(['node',str(root/'test_batch.cjs')],check=True)
with tempfile.TemporaryDirectory(prefix='compact-opaque-') as tmp:
    target=Path(tmp)/'test-compact'
    subprocess.run([os.environ.get('CC','cc'),'-std=c11','-O2','-Wall','-Wextra','-Werror','-pedantic',str(root/'compact_opaque.c'),str(root/'test_compact.c'),'-lm','-o',str(target)],check=True)
    subprocess.run([str(target)],check=True)
