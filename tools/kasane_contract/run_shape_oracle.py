"""pocket.shapes (main/pocket/pocket_shape.c) against the JS WAKE it replaced.

Builds test_shape_oracle.c twice -- the C in double (must reproduce the JS's
draws bit for bit) and in float (the device build: reports the difference) --
and runs both with the reference chunks from git (--baseline-rev, the last
commit whose WAKE drew in JS) against the tree's. Host only.
docs/apps/megademo-shapes.md.
"""
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
QJS = ROOT / "components/quickjs-ng/quickjs-ng"
OUT = ROOT / ".cache/shape_oracle"
CHUNKS = ("megademo_act1.js", "megademo_act2.js", "megademo_wake.js")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline-rev", default="c52af92",
                        help="commit whose WAKE drew in JS (the reference)")
    args = parser.parse_args()
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not cc:
        raise SystemExit("No host C compiler found")
    OUT.mkdir(parents=True, exist_ok=True)
    ref = []
    for name in CHUNKS:
        text = subprocess.run(["git", "show", f"{args.baseline_rev}:apps/kasane/{name}"], cwd=ROOT,
                              check=True, capture_output=True, text=True).stdout
        path = OUT / f"ref_{name}"
        path.write_text(text, encoding="utf-8")
        ref.append(str(path))
    cand = [str(ROOT / "apps/kasane" / name) for name in CHUNKS]
    sources = [*(QJS / n for n in ("dtoa.c", "libregexp.c", "libunicode.c", "quickjs.c", "quickjs-vm.c")),
               ROOT / "main/pocket/pocket_shape.c", ROOT / "tools/hostshim/pocket_api_stub.c",
               ROOT / "tools/kasane_contract/shape_draw_shim.c",
               ROOT / "tools/kasane_contract/test_shape_oracle.c"]
    failed = 0
    for mode in ("double", "float"):
        binary = OUT / f"test_shape_oracle_{mode}"
        command = [cc, "-std=gnu11", "-O2", "-ffp-contract=off", "-DQUICKJS_NG_BUILD", "-D_GNU_SOURCE",
                   *(["-DPOCKET_SHAPE_DOUBLE"] if mode == "double" else []),
                   "-I", str(QJS), "-I", str(ROOT / "main/pocket"), "-I", str(ROOT / "main/ui/kasane"),
                   "-I", str(ROOT / "main"), "-I", str(ROOT / "tools/hostshim"),
                   "-I", str(ROOT / "tools/kasane_contract"),
                   *map(str, sources), "-lm", "-o", str(binary)]
        subprocess.run(command, cwd=ROOT, check=True)
        failed |= subprocess.run([str(binary), ",".join(ref), ",".join(cand)], cwd=ROOT).returncode
    return failed


if __name__ == "__main__":
    sys.exit(main())
