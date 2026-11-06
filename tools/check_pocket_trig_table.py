"""pocket_trig.h's sine table must be Shiki's (main/ui/kasane/ksn_shiki.c):
the firmware links one copy (ksn_shiki.c aliases it), hosts without Shiki use
the header's. docs/vm/qjs-observe.md sec.18.

    python3 tools/check_pocket_trig_table.py
"""
import math, re, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def table(path, name):
    text = (ROOT / path).read_text()
    m = re.search(re.escape(name) + r'\[257\]\s*=\s*\{([^}]*)\}', text)
    if not m:
        sys.exit(f'{path}: {name}[257] not found')
    return [int(v) for v in re.findall(r'-?\d+', m.group(1))]


a = table('components/quickjs-ng/quickjs-ng/pocket_trig.h', 'pocket_sin_q16')
b = table('main/ui/kasane/ksn_shiki.c', 'ksn_shiki_sin_table')
ref = [round(math.sin(k * math.pi / 512) * 65536) for k in range(257)]
bad = [k for k in range(257) if not (a[k] == b[k] == ref[k])]
if len(a) != 257 or len(b) != 257 or bad:
    sys.exit(f'POCKET_TRIG_TABLE FAIL: {len(a)} / {len(b)} entries, differ at {bad[:8]}')
print('POCKET_TRIG_TABLE PASS: 257 entries, pocket_trig.h == ksn_shiki.c == round(sin(k pi/512) * 65536)')
