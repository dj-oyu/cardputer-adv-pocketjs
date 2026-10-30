"""Build the real QuickJS host harness and compare JS/C megademo over 48 frames."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path


from megademo_chunks import chunk_arg
ROOT = Path(__file__).resolve().parents[2]
QJS = ROOT / "components/quickjs-ng/quickjs-ng"
OUT = ROOT / ".cache/kasane_megademo_js"


def compiler() -> str:
    for name in ("cc", "gcc", "clang"):
        found = shutil.which(name)
        if found:
            return found
    mingw = Path("C:/msys64/ucrt64/bin/gcc.exe")
    if mingw.exists():
        return str(mingw)
    raise SystemExit("No host C compiler found")


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    binary = OUT / ("test_proc_megademo_js.exe" if sys.platform == "win32" else "test_proc_megademo_js")
    cc = compiler()
    sources = [
        *(QJS / name for name in ("dtoa.c", "libregexp.c", "libunicode.c", "quickjs.c", "quickjs-vm.c")),
        ROOT / "main/ui/kasane/ksn_procedural.c",
        ROOT / "main/ui/kasane/ksn_proc_analysis.c",
        ROOT / "main/ui/kasane/ksn_proc_plan.c",
        ROOT / "main/pocket/pocket_shape.c", ROOT / "main/ayatsuri/ayatsuri_model.c", ROOT / "tools/hostshim/pocket_api_stub.c",
        ROOT / "tools/kasane_contract/shape_draw_shim.c",
        ROOT / "tools/kasane_contract/test_proc_megademo_js.c",
    ]
    command = [
        cc, "-std=gnu11", "-O2", "-DQUICKJS_NG_BUILD", "-D_GNU_SOURCE",
        "-I", str(QJS), "-I", str(ROOT / "main/ui/kasane"), "-ffp-contract=off",
        "-I", str(ROOT / "main/pocket"), "-I", str(ROOT / "main"), "-I", str(ROOT / "tools/hostshim"),
        "-I", str(ROOT / "tools/kasane_contract"),
        *(str(source) for source in sources), "-lm", "-o", str(binary),
    ]
    env = os.environ.copy()
    env["PATH"] = str(Path(cc).parent) + os.pathsep + env.get("PATH", "")
    subprocess.run(command, cwd=ROOT, env=env, check=True)
    # The app's chunks, then Act I's former programs (the VM reference).
    ref = ROOT / "tools/kasane_contract/proc_megademo_news_ref.js"
    subprocess.run([str(binary), chunk_arg() + "," + str(ref)],
                   cwd=ROOT, env=env, check=True)


if __name__ == "__main__":
    main()
