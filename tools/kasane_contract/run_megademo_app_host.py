"""Run MEGADEMO through the real QuickJS and the real pocket.kasane on the host.

Builds tools/kasane_contract/test_megademo_app_host.c from the source list of
tools/build_kasane_test.sh plus the grid resize and pixel adapters the app
uses (that script alone does not link them), then runs:
  1. the current apps/kasane/proc_megademo.js: two loops, zoom, tier changes,
     scene skips; plan-loader counts and guest heap;
  2. the app as committed at --baseline-rev for the same guest-heap
     measurement, so the growth is a same-harness difference.
--m32 builds everything for i386 with the device's 8-byte JSValue and 4-byte
pointers (tools/vmtest/m32_sysroot.sh), so object sizes match the firmware;
the default 64-bit build runs with ASan/UBSan. WSL/Linux only. No device.
"""
from __future__ import annotations

import argparse
import os
import re
import struct
import subprocess
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CACHE = ROOT / ".cache/kasane_megademo_app"
QJS = "components/quickjs-ng/quickjs-ng"
EXTRA = ["main/pocket/pocket_grid.c", "main/ui/kasane/ksn_proc_grid.c",
         "main/ui/kasane/ksn_proc_grid_pie.c", "main/ui/kasane/ksn_proc_grid_image.c",
         "main/ui/kasane/ksn_proc_grid_resize.c", "main/pocket/pocket_pixel.c",
         "main/ui/kasane/ksn_pixel_function.c", "main/ui/kasane/ksn_pixel_span.c"]


def kasane_sources() -> list[str]:
    """The .c list of tools/build_kasane_test.sh (minus the test and QuickJS)."""
    text = (ROOT / "tools/build_kasane_test.sh").read_text()
    return [s for s in re.findall(r"(?:main|tools)/[\w/]+\.c", text)
            if not s.startswith("tools/test_pocket_kasane")]


def build(flags: list[str], cache: Path) -> Path:
    cache.mkdir(parents=True, exist_ok=True)
    subprocess.run(["python3", "tools/make_font.py", str(cache)], cwd=ROOT, check=True,
                   stdout=subprocess.DEVNULL)
    data = (ROOT / "apps/pet/assets/pets-compact.bin").read_bytes()
    (cache / "kasane_pet_test_data.h").write_text(
        "static const uint8_t pet_test_data[] = {" + ",".join(map(str, data)) + "};\n")
    defs = ["-DQUICKJS_NG_BUILD", "-D_GNU_SOURCE"]
    objects = []
    for name in ("dtoa", "libregexp", "libunicode", "quickjs", "quickjs-vm"):
        obj = cache / f"{name}.o"
        src = ROOT / QJS / f"{name}.c"
        if not obj.exists() or src.stat().st_mtime > obj.stat().st_mtime:
            subprocess.run(["gcc", "-std=gnu11", "-c", "-O1", "-g", "-w", *flags, *defs, "-I", QJS,
                            "-I", "components/pocketjs_guest/include", str(src), "-o", str(obj)],
                           cwd=ROOT, check=True)
        objects.append(str(obj))
    binary = cache / "test-megademo-app"
    includes = [f"-I{d}" for d in (QJS, "tools/hostshim", "main", "main/pocket", "main/ui",
                                   "main/ui/kasane", "main/text", "main/hal", str(cache))]
    subprocess.run(["gcc", "-std=gnu11", "-O1", "-g", *flags, "-DKSN_GRID_PIE_MODEL",
                    "-Wall", "-Wextra", "-Werror", "-fno-omit-frame-pointer", *includes,
                    "tools/kasane_contract/test_megademo_app_host.c", *kasane_sources(), *EXTRA,
                    *objects, "-Wl,--wrap=calloc", "-Wl,--wrap=free", "-lm", "-o", str(binary)],
                   cwd=ROOT, check=True)
    return binary


def panel_sheet(folder: Path, columns: int = 4) -> Path:
    """The composited panels written by MEGA_PPM, in tick order, as one PNG."""
    files = sorted(folder.glob("app_*.ppm"))
    header = b"P6\n240 135\n255\n"
    images = [f.read_bytes()[len(header):] for f in files]
    w, h, gap = 240, 135, 2
    rows = (len(images) + columns - 1) // columns
    width, height = columns * w + (columns - 1) * gap, rows * h + (rows - 1) * gap
    grey = bytes((0x30, 0x30, 0x30))
    lines = bytearray()
    for y in range(height):
        lines.append(0)
        row, sy = divmod(y, h + gap)
        for column in range(columns):
            i = row * columns + column
            if sy >= h or i >= len(images):
                lines.extend(grey * w)
            else:
                lines.extend(images[i][sy * w * 3:(sy + 1) * w * 3])
            if column + 1 < columns:
                lines.extend(grey * gap)
    chunk = lambda tag, data: (struct.pack(">I", len(data)) + tag + data +
                               struct.pack(">I", zlib.crc32(tag + data)))
    out = folder / "sheet_app_composited.png"
    out.write_bytes(bytes((0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a)) +
                    chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
                    chunk(b"IDAT", zlib.compress(bytes(lines), 9)) + chunk(b"IEND", b""))
    return out


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-rev", default="HEAD")
    parser.add_argument("--ppm", type=Path, help="write composited panels here")
    parser.add_argument("--m32", action="store_true", help="device-sized i386 build, no sanitizers")
    args = parser.parse_args()
    if args.m32:
        # A sysroot under .cache survives WSL restarts, which empty /tmp.
        sysroot = dict(os.environ, M32_SYSROOT=str(CACHE / "m32sys"))
        flags = subprocess.run(["bash", "tools/vmtest/m32_sysroot.sh"], cwd=ROOT, check=True, env=sysroot,
                               capture_output=True, text=True).stdout.split()
        binary = build(flags, CACHE / "m32")
    else:
        binary = build(["-fsanitize=address,undefined"], CACHE / "asan")
    env = os.environ.copy()
    # Boot-lifetime singletons in the Kasane fixture are intentionally kept.
    env.setdefault("ASAN_OPTIONS", "detect_leaks=0:abort_on_error=1")
    env.setdefault("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1")
    if args.ppm:
        args.ppm.mkdir(parents=True, exist_ok=True)
        env["MEGA_PPM"] = str(args.ppm.resolve())
    subprocess.run([str(binary), "apps/kasane/proc_megademo.js"], cwd=ROOT, env=env, check=True)
    if args.ppm:
        print(f"Wrote {panel_sheet(args.ppm.resolve())}")
    old = CACHE / "proc_megademo_baseline.js"
    shown = subprocess.run(["git", "show", f"{args.baseline_rev}:apps/kasane/proc_megademo.js"],
                           cwd=ROOT, capture_output=True)
    # WSL git cannot open a Windows-created worktree; extract it there first:
    #   git show <rev>:apps/kasane/proc_megademo.js > .cache/kasane_megademo_app/proc_megademo_baseline.js
    if shown.returncode == 0:
        old.write_bytes(shown.stdout)
    elif not old.exists():
        raise SystemExit(f"cannot read the baseline app: {shown.stderr.decode().strip()}")
    env.pop("MEGA_PPM", None)
    print(f"baseline: {old.name} ({old.stat().st_size} B)")
    subprocess.run([str(binary), str(old), "--baseline"], cwd=ROOT, env=env, check=True)


if __name__ == "__main__":
    main()
