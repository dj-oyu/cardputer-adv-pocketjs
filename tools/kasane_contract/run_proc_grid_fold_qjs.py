"""Exercise symbolic JS folds through real QuickJS and the typed grid compiler."""

import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
QJS = ROOT / "components/quickjs-ng/quickjs-ng"
OUT = ROOT / ".cache/kasane-grid-fold-qjs"


def main(trace_path=None, fused=False):
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not cc:
        candidate = Path("C:/msys64/ucrt64/bin/gcc.exe")
        if candidate.exists():
            cc = str(candidate)
    if not cc:
        raise SystemExit("No host C compiler found")
    OUT.mkdir(parents=True, exist_ok=True)
    binary = OUT / ("test_proc_grid_fold_qjs" + (".exe" if sys.platform == "win32" else ""))
    sources = [*(QJS / name for name in
                 ("dtoa.c", "libregexp.c", "libunicode.c", "quickjs.c", "quickjs-vm.c")),
               ROOT / "main/ui/kasane/ksn_proc_grid.c",
               ROOT / "main/ui/kasane/ksn_proc_grid_pie.c",
               ROOT / "main/ui/kasane/ksn_proc_points.c",
               ROOT / "tools/kasane_contract/test_proc_grid_fold_qjs.c"]
    command = [cc, "-std=gnu11", "-O2", "-DQUICKJS_NG_BUILD", "-D_GNU_SOURCE",
               "-DKSN_GRID_PIE_MODEL", "-I", str(QJS),
               "-I", str(ROOT / "main/ui/kasane"),
               *(str(source) for source in sources), "-lm", "-o", str(binary)]
    env = os.environ.copy()
    env["PATH"] = str(Path(cc).parent) + os.pathsep + env.get("PATH", "")
    subprocess.run(command, cwd=ROOT, env=env, check=True)
    args = [str(binary), str(ROOT / "apps/kasane/grid_fold.js"),
            str(ROOT / "apps/kasane/grid_fold_examples.js")]
    if trace_path is not None:
        args.append(str(trace_path))
        if fused:
            args.append("fused")
    subprocess.run(args,
                   cwd=ROOT, env=env, check=True)


if __name__ == "__main__":
    main()
