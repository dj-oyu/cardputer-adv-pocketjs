"""Generate the baked sound tables and diff them against the old synthesis.

    python tools/test_sfx.py

Generates sfx_tables.h into .cache/sfx exactly as the build does, compiles
tools/test_sfx_tables.c against it and against main/hal/sfx_synth.h, and prints
the worst per-sample difference. No device, no serial port.
"""
import subprocess
import sys
from pathlib import Path

import make_sfx

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / '.cache' / 'sfx'

make_sfx.generate(OUT)
binary = OUT / ('check.exe' if sys.platform == 'win32' else 'check')
subprocess.run(
    ['gcc', '-O2', '-Wall', '-Wextra', '-Werror',
     '-I', str(OUT), '-I', str(ROOT / 'main' / 'hal'),
     str(ROOT / 'tools' / 'test_sfx_tables.c'), '-lm', '-o', str(binary)],
    check=True)
sys.exit(subprocess.run([str(binary)]).returncode)
