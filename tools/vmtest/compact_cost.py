"""What compact variables save per app (host, WSL/Linux only).

    python3 tools/vmtest/compact_cost.py [DIR_WITH_DERBY_CHUNKS]

Compiles each shipping app's scripts (tools/vmtest/strip_cost.py's list)
with tools/vmtest/retain_cost.c twice in one -m32 build made like the
firmware's (QJS_DISABLE_DUMPS, the device's object sizes): in a plain
context (src: full JSVarDefs and local names, as before) and in a hardened
one (harden: what the device compiles since docs/vm/compact-vars.md), and
prints the charged bytes (TLSF lengths) the functions keep. Host
measurements of the device's allocation sizes, not device ones.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import strip_cost  # noqa: E402

ROOT = strip_cost.ROOT


def main():
    binary = strip_cost.build('fw', ['-DQJS_DISABLE_DUMPS'])
    derby = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'apps/derby'
    apps = dict(strip_cost.APPS, **{"DERBY WATCH": [str(derby / c) for c in strip_cost.DERBY]})
    print(f"{'app':<14} {'source B':>9} {'plain B':>8} {'hardened B':>10} {'saved B':>8}")
    total = 0
    for name, fs in apps.items():
        src = sum((ROOT / f).stat().st_size for f in fs)
        a = strip_cost.charged_mode(binary, 'src', fs)
        b = strip_cost.charged_mode(binary, 'harden', fs)
        total += a - b
        print(f"{name:<14} {src:9d} {a:8d} {b:10d} {a - b:8d}")
    print(f"{'all':<14} {'':9} {'':8} {'':10} {total:8d}")


if __name__ == '__main__':
    main()
