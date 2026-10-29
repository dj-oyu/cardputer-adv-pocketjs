"""MEGADEMO's chunk files, in apps/kasane/chunks.txt's order.

The app is an entry (apps/kasane/proc_megademo.js) that pocket.app.load()s
these on the device. The host tests evaluate the same files directly, one
JS_Eval each in one realm, which is what those loads do. The view chunks
need pocket.kasane; a test without it takes act_only=True.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VIEW = ('view', 'set', 'loop')


def chunk_files(act_only=False):
    out = []
    for raw in (ROOT / 'apps/kasane/chunks.txt').read_text(encoding='utf-8').splitlines():
        words = raw.split('#', 1)[0].split()
        if len(words) != 2 or words[0] == 'app' or act_only and words[0] in VIEW:
            continue
        out.append(ROOT / 'apps/kasane' / words[1])
    return out


def chunk_arg(act_only=False):
    """The C harnesses' first argument: the files, comma-separated."""
    return ','.join(str(p) for p in chunk_files(act_only))
