"""pocket.shapes (main/pocket/pocket_shape.c) against its contract,
docs/api/shapes.md: tools/kasane_contract/shape_contract.js run by
test_pocket_shape.c, built with the host's C compiler and the tree's QuickJS.
Host only."""
from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
QJS = ROOT / "components/quickjs-ng/quickjs-ng"
OUT = ROOT / ".cache/pocket_shape"


def main() -> int:
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not cc:
        raise SystemExit("No host C compiler found")
    OUT.mkdir(parents=True, exist_ok=True)
    binary = OUT / "test_pocket_shape"
    sources = [*(QJS / n for n in ("dtoa.c", "libregexp.c", "libunicode.c", "quickjs.c", "quickjs-vm.c")),
               ROOT / "main/pocket/pocket_shape.c", ROOT / "main/ayatsuri/ayatsuri_model.c", ROOT / "tools/hostshim/pocket_api_stub.c",
               ROOT / "tools/kasane_contract/test_pocket_shape.c"]
    subprocess.run([cc, "-std=gnu11", "-O1", "-g", "-ffp-contract=off", "-fsanitize=address,undefined",
                    "-DQUICKJS_NG_BUILD", "-D_GNU_SOURCE", "-I", str(QJS), "-I", str(ROOT / "main/pocket"),
                    "-I", str(ROOT / "main"), "-I", str(ROOT / "main/ui/kasane"), "-I", str(ROOT / "tools/hostshim"),
                    *map(str, sources), "-lm", "-o", str(binary)], cwd=ROOT, check=True)
    return subprocess.run([str(binary), str(ROOT / "tools/kasane_contract/shape_contract.js")], cwd=ROOT,
                          env={"ASAN_OPTIONS": "detect_leaks=0"}).returncode


if __name__ == "__main__":
    sys.exit(main())
