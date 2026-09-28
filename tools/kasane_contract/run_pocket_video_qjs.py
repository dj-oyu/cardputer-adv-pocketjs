"""Build the video frame adapter with real QuickJS on the host."""
from pathlib import Path
import os
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
QJS = ROOT / "components/quickjs-ng/quickjs-ng"
OUT = ROOT / ".cache/kasane-pocket-video-qjs"


def main():
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not cc and Path("C:/msys64/ucrt64/bin/gcc.exe").exists():
        cc = "C:/msys64/ucrt64/bin/gcc.exe"
    if not cc:
        raise SystemExit("No host C compiler")
    OUT.mkdir(parents=True, exist_ok=True)
    binary = OUT / ("test_pocket_video_qjs" + (".exe" if sys.platform == "win32" else ""))
    sources = [*(QJS / name for name in
                 ("dtoa.c", "libregexp.c", "libunicode.c", "quickjs.c", "quickjs-vm.c")),
               ROOT / "main/ui/kasane/ksn_video_frames.c",
               ROOT / "main/ui/kasane/ksn_core.c",
               ROOT / "main/ui/kasane/ksn_render.c",
               ROOT / "main/ui/kasane/ksn_blend_pie.c",
               ROOT / "main/pocket/pocket_video.c",
               ROOT / "tools/kasane_contract/test_pocket_video_qjs.c"]
    command = [cc, "-std=gnu11", "-O2", "-DQUICKJS_NG_BUILD", "-D_GNU_SOURCE",
               "-I", str(QJS), "-I", str(ROOT / "main"),
               "-I", str(ROOT / "main/pocket"),
               "-I", str(ROOT / "main/ui/kasane"),
               "-I", str(ROOT / "tools/kasane_contract"),
               "-I", str(ROOT / "tools/hostshim"),
               *(str(s) for s in sources), "-lm", "-o", str(binary)]
    env = os.environ.copy()
    env["PATH"] = str(Path(cc).parent) + os.pathsep + env.get("PATH", "")
    subprocess.run(command, cwd=ROOT, env=env, check=True)
    subprocess.run([str(binary)], cwd=ROOT, env=env, check=True)


if __name__ == "__main__":
    main()
