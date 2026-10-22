"""Execute native control core and actual shared B/C transport; no hardware."""
import os
import re
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run():
    registry = (ROOT / "main/pocket/app_registry.c").read_text()
    owner = (ROOT / "main/main.c").read_text()
    registered = set(re.findall(r'\.id="(local\.[^"]+)"', registry)) | {"local.work"}
    described = set(re.findall(r'NATIVE_APP\("(local\.[^"]+)"', owner))
    assert registered <= described, f"Known manifests missing descriptors: {registered - described}"
    bridge = (ROOT / "main/pocket/pocket_bridge.c").read_text()
    transport = bridge[:bridge.index("// ------------------------------------------------------------------ the link")]
    transport = "\n".join(line for line in transport.splitlines() if not line.startswith("#include"))
    with tempfile.TemporaryDirectory(prefix="app-control-") as directory:
        out = Path(directory)
        (out / "app_control_transport.inc").write_text(transport)
        for flags in (["-O2", "-fstrict-aliasing"], ["-O1", "-g", "-fsanitize=address,undefined"]):
            exe = out / "control"
            subprocess.run([
                "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wpedantic", *flags,
                "-I", str(out), "-I", str(ROOT / "main/pocket"),
                str(ROOT / "main/pocket/app_control.c"), str(ROOT / "tools/test_app_control.c"),
                "-o", str(exe),
            ], check=True)
            subprocess.run([str(exe)], check=True, env=dict(
                os.environ, ASAN_OPTIONS="detect_leaks=0:abort_on_error=1",
                UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1",
            ))


if __name__ == "__main__":
    run()
