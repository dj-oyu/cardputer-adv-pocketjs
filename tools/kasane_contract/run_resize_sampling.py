"""Measure fixed-window sampling cost and quality with the real bilinear span."""
from pathlib import Path
import os
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / ".cache/kasane-resize-sampling"


def main():
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not cc and Path("C:/msys64/ucrt64/bin/gcc.exe").exists():
        cc = "C:/msys64/ucrt64/bin/gcc.exe"
    if not cc:
        raise SystemExit("No host C compiler")
    OUT.mkdir(parents=True, exist_ok=True)
    binary = OUT / ("sampling" + (".exe" if sys.platform == "win32" else ""))
    command = [cc, "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
               "-DKASANE_PROC_DEVICE_PROBE", "-DKSN_GRID_PIE_MODEL",
               "-I", str(ROOT / "main"),
               str(ROOT / "main/ui/kasane/ksn_proc_grid_resize.c"),
               str(ROOT / "tools/kasane_contract/test_resize_sampling.c"),
               "-lm", "-o", str(binary)]
    env = os.environ.copy()
    env["PATH"] = str(Path(cc).parent) + os.pathsep + env.get("PATH", "")
    subprocess.run(command, cwd=ROOT, env=env, check=True)
    subprocess.run([str(binary)], cwd=ROOT, env=env, check=True)


if __name__ == "__main__":
    main()
