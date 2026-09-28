"""Build and run the 48-frame MEGADEMO sampling quality comparison."""

from pathlib import Path
import os
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / ".cache/kasane-d3d-sampling"


def main() -> None:
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not cc and Path("C:/msys64/ucrt64/bin/gcc.exe").exists():
        cc = "C:/msys64/ucrt64/bin/gcc.exe"
    if not cc:
        raise SystemExit("No host C compiler")
    OUT.mkdir(parents=True, exist_ok=True)
    binary = OUT / ("megademo-temporal" + (".exe" if sys.platform == "win32" else ""))
    command = [cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
               "-I", str(ROOT / "main"),
               "-I", str(ROOT / "main/ui/kasane"),
               "-I", str(ROOT / "tools/kasane_contract"),
               *[str(ROOT / "main/ui/kasane" / name) for name in
                 ("ksn_procedural.c", "ksn_proc_analysis.c",
                  "ksn_proc_plan.c", "ksn_proc_grid_resize.c")],
               str(ROOT / "tools/kasane_contract/test_megademo_temporal_sampling.c"),
               "-lm", "-o", str(binary)]
    env = os.environ.copy()
    env["PATH"] = str(Path(cc).parent) + os.pathsep + env.get("PATH", "")
    subprocess.run(command, cwd=ROOT, env=env, check=True)
    subprocess.run([str(binary)], cwd=ROOT, env=env, check=True)


if __name__ == "__main__":
    main()
