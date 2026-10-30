"""Continuous analytic material patches: actual renderer O0/O2, host only."""
from __future__ import annotations

import os
from pathlib import Path
import subprocess

from run_proc_megademo_scenes import compiler

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / ".cache/proc_ellipsoid"


def main() -> None:
    cc = compiler()
    OUT.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env["PATH"] = str(Path(cc).parent) + os.pathsep + env.get("PATH", "")
    src = ROOT / "main/ui/kasane"
    for test, opt in ((name, opt) for name in ("ellipsoid", "blend_ellipsoids", "clipped_ellipsoid") for opt in ("0", "2")):
        binary = OUT / (f"{test}_O{opt}" + (".exe" if os.name == "nt" else ""))
        flags = [] if os.name == "nt" else ["-fsanitize=address,undefined"]
        subprocess.run([cc, "-std=gnu11", f"-O{opt}", "-g", "-Wall", "-Wextra", "-Werror",
                        "-ffp-contract=off", *flags, "-I", str(src),
                        str(ROOT / f"tools/kasane_contract/test_proc_{test}.c"),
                        str(src / "ksn_procedural.c"), str(src / "ksn_procedural_surface.c"),
                        "-lm", "-o", str(binary)], cwd=ROOT, env=env, check=True)
        subprocess.run([str(binary)], cwd=ROOT, env=env, check=True)
        print(f"{test.upper()} O{opt} PASS" + (" (ASan/UBSan)" if flags else " (native Windows)"))


if __name__ == "__main__":
    main()
