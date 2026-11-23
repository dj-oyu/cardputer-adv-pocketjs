"""The pinned engine that the A/B checks compare the current one against.

QuickJS is the submodule components/quickjs-ng/quickjs-ng (the fork
dj-oyu/quickjs), so a pinned engine is a fork commit, read from the
submodule's own history.

0d281e1 is the fork's upstream v0.18.0 merge (55b82c7, pocketjs 50b5fe4e,
docs/vm/upstream-v018.md) plus one deliberate change: a stripped build
(CONFIG_QJS_LEAN_STRIP_DEBUG or NDEBUG) no longer emits OP_source_loc, so it
allocates less and the --fail-alloc targets move in strip builds (the strip
variants' expectations are not enforced, function_allocation/run.py). The
pin before it, pocketjs dd36a29 (Math.sin/cos/tan from Shiki's sine table),
was v0.14: its quickjs.c no longer compiles against v0.18's quickjs.h
(JS_NewArrayBuffer and the interrupt handler changed). Crossing to v0.18 was
checked once by running mixed_numeric on both trees
(docs/vm/upstream-v018.md sec.7).
"""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
QJS = ROOT / 'components/quickjs-ng/quickjs-ng'
ENGINE_BASE = '0d281e1de665d57fa42496f01d18bdfcf2b3a391'


def _renamed(data):
    """The pinned file with the fork's 2026-11-23 rename applied (c7d48fd:
    POCKET_VM_* -> QJS_LEAN_*, JS_Pocket* -> JS_Lean*, pocket_trig.h ->
    lean_trig.h, ...). The pin predates the rename, and its quickjs.c is
    compiled against the current headers and harness; the rename is
    mechanical (the firmware image was byte-identical across it), so the
    renamed pin is still the pinned engine."""
    import sys
    sys.path.insert(0, str(ROOT / 'tools/rename'))
    from collections import Counter
    import kobako_rename
    text = data.decode('utf-8')
    return kobako_rename.rewrite(text, kobako_rename.build_rules(True), Counter(), [], 'pin').encode('utf-8')


def base_file(name):
    """Bytes of quickjs-ng/<name> at ENGINE_BASE, with the rename applied."""
    have = subprocess.run(['git', '-C', str(QJS), 'cat-file', '-e', ENGINE_BASE + '^{commit}'],
                          capture_output=True).returncode == 0
    if not have:  # a shallow submodule checkout
        subprocess.run(['git', '-C', str(QJS), 'fetch', '--depth=1', 'origin', ENGINE_BASE], check=True)
    return _renamed(subprocess.check_output(['git', '-C', str(QJS), 'show', f'{ENGINE_BASE}:{name}']))
