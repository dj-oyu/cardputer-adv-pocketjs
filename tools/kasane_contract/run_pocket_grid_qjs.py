"""Compile the ordinary app grid adapter against real QuickJS and PIE model."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
QJS = ROOT / "components/quickjs-ng/quickjs-ng"
OUT = ROOT / ".cache/kasane-pocket-grid-qjs"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true",
                        help="run the functional suite under AddressSanitizer and UBSan")
    parser.add_argument("--scoped-only", action="store_true",
                        help="run scoped functional inputs without injected I/O/ACK failures")
    parser.add_argument("--out", type=Path, default=OUT,
                        help="isolated host build directory")
    args = parser.parse_args()
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not cc and Path("C:/msys64/ucrt64/bin/gcc.exe").exists():
        cc = "C:/msys64/ucrt64/bin/gcc.exe"
    if not cc:
        raise SystemExit("No host C compiler")
    args.out.mkdir(parents=True, exist_ok=True)
    binary = args.out.resolve() / ("test_pocket_grid_qjs" + ("_asan" if args.sanitize else "") +
                    (".exe" if sys.platform == "win32" else ""))
    sources = [*(QJS / name for name in
                 ("dtoa.c", "libregexp.c", "libunicode.c", "quickjs.c", "quickjs-vm.c")),
               ROOT / "main/ui/kasane/ksn_proc_grid.c",
               ROOT / "main/ui/kasane/ksn_proc_grid_pie.c",
               ROOT / "main/ui/kasane/ksn_proc_grid_image.c",
               ROOT / "main/ui/kasane/ksn_proc_grid_resize.c",
               ROOT / "main/pocket/pocket_grid.c",
               ROOT / "tools/kasane_contract/test_pocket_grid_qjs.c"]
    flags = (["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
             if args.sanitize else ["-O2"])
    if args.sanitize and sys.platform == "linux":
        flags += ["-fno-pie", "-no-pie"]
    command = [cc, "-std=gnu11", *flags, "-DQUICKJS_NG_BUILD", "-D_GNU_SOURCE",
               "-DKSN_GRID_PIE_MODEL", "-DKSN_GRID_APP_HOST_TEST",
               "-I", str(QJS), "-I", str(ROOT / "main"),
               "-I", str(ROOT / "main/pocket"),
               "-I", str(ROOT / "main/ui/kasane"),
               "-I", str(ROOT / "tools/hostshim"),
               *(str(s) for s in sources), "-lm", "-o", str(binary)]
    env = os.environ.copy()
    env["PATH"] = str(Path(cc).parent) + os.pathsep + env.get("PATH", "")
    if args.sanitize:
        env.setdefault("ASAN_OPTIONS", "detect_leaks=1")
        env.setdefault("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1")
    subprocess.run(command, cwd=ROOT, env=env, check=True)
    subprocess.run([str(binary), *(["--scoped-only"] if args.scoped_only else []),
                    str(ROOT / "apps/kasane/grid_fold.js"),
                    str(ROOT / "apps/kasane/grid_fold_device_probe.js"),
                    str(ROOT / "tools/kasane_contract/test_pocket_grid_scoped_inputs.js")],
                   cwd=ROOT, env=env, check=True)


if __name__ == "__main__":
    main()
