"""Play BIG WAVE on the host: real QuickJS, real pocket.kasane, real input.keys.

Builds tools/games/test_big_wave_host.c with the source list of
tools/build_kasane_test.sh plus pocket.input (keymap, keystate) and the Q14
point PIE model, runs the scripted play and the key-driven bot (see the C
file's header for what is checked), and with --sheet writes the preview contact
sheet (docs/apps/big-wave-preview.png by default) from the labelled frames.

  python3 tools/games/run_big_wave.py [--sheet [PATH]] [--m32] [--heap-limit 163840]

WSL/Linux only. No device or serial port is used.
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
CACHE = ROOT / ".cache/bigwave_host"
QJS = "components/quickjs-ng/quickjs-ng"
EXTRA = ["main/pocket/pocket_input.c", "main/hal/keymap.c", "main/hal/keystate.c"]
SHEET = ["02_ride", "04_turn_left", "05_turn_right", "06_air", "07_tube", "08_wipeout_lip",
         "10_wipeout_splash", "14_dusk", "15_clear"]


def kasane_sources() -> list[str]:
    """The .c list of tools/build_kasane_test.sh (minus its default test)."""
    text = (ROOT / "tools/build_kasane_test.sh").read_text()
    return [s for s in re.findall(r"(?:main|tools)/[\w/]+\.c", text)
            if not s.startswith("tools/test_pocket_kasane")]


def build(m32: bool) -> Path:
    cache = CACHE / ("m32" if m32 else "asan")
    cache.mkdir(parents=True, exist_ok=True)
    if m32:
        # The device's 8-byte JSValue and 4-byte pointers (tools/vmtest/m32_sysroot.sh).
        sysroot = dict(os.environ, M32_SYSROOT=str(CACHE / "m32sys"))
        flags = subprocess.run(["bash", "tools/vmtest/m32_sysroot.sh"], cwd=ROOT, check=True, env=sysroot,
                               capture_output=True, text=True).stdout.split()
    else:
        flags = ["-fsanitize=address,undefined"]
    subprocess.run(["python3", "tools/make_font.py", str(cache)], cwd=ROOT, check=True,
                   stdout=subprocess.DEVNULL)
    pets = (ROOT / "apps/pet/assets/pets-compact.bin").read_bytes()
    (cache / "kasane_pet_test_data.h").write_text(
        "static const uint8_t pet_test_data[] = {" + ",".join(map(str, pets)) + "};\n")
    api = (ROOT / "main/pocket/pocket_api.c").read_text()
    a = api.index("// ----------------------------------------------------------- subscriptions")
    b = api.index("// ------------------------------------------------------- async completions", a)
    (cache / "pocket_sub_impl.inc").write_text(api[a:b])
    defs = ["-DQUICKJS_NG_BUILD", "-D_GNU_SOURCE"]
    objects = []
    for name in ("dtoa", "libregexp", "libunicode", "quickjs", "quickjs-vm"):
        obj, src = cache / f"{name}.o", ROOT / QJS / f"{name}.c"
        if not obj.exists() or src.stat().st_mtime > obj.stat().st_mtime:
            subprocess.run(["gcc", "-std=gnu11", "-c", "-O1", "-g", "-w", *flags, *defs, "-I", QJS,
                            "-I", "components/pocketjs_guest/include", str(src), "-o", str(obj)],
                           cwd=ROOT, check=True)
        objects.append(str(obj))
    binary = cache / "test-big-wave"
    includes = [f"-I{d}" for d in (QJS, "tools/hostshim", "main", "main/pocket", "main/ui",
                                   "main/ui/kasane", "main/text", "main/hal", str(cache))]
    subprocess.run(["gcc", "-std=gnu11", "-O1", "-g", *flags, "-DKSN_PROC_POINTS_PIE_MODEL",
                    "-Wall", "-Wextra", "-Werror", "-fno-omit-frame-pointer", *includes,
                    "tools/games/test_big_wave_host.c", *kasane_sources(), *EXTRA, *objects,
                    "-Wl,--wrap=calloc", "-Wl,--wrap=free", "-lm", "-o", str(binary)],
                   cwd=ROOT, check=True)
    return binary


def read_ppm(path: Path) -> bytes:
    data = path.read_bytes()
    header = b"P6\n240 135\n255\n"
    return data[len(header):]


def sheet(folder: Path, out: Path, columns: int = 3) -> None:
    images = [read_ppm(folder / f"{name}.ppm") if (folder / f"{name}.ppm").exists() else bytes(240 * 135 * 3)
              for name in SHEET]
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
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(b"\x89PNG\r\n\x1a\n" +
                    chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
                    chunk(b"IDAT", zlib.compress(bytes(lines), 9)) + chunk(b"IEND", b""))
    print(f"Wrote {out} ({width}x{height}, {out.stat().st_size} B)")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sheet", nargs="?", const=ROOT / "docs/apps/big-wave-preview.png", type=Path)
    parser.add_argument("--m32", action="store_true", help="device-sized i386 build, no sanitizers")
    parser.add_argument("--heap-limit", type=int, help="guest memory limit, e.g. 163840 as on the device")
    parser.add_argument("--source", default="apps/bigwave/big_wave.js")
    args = parser.parse_args()
    binary = build(args.m32)
    env = os.environ.copy()
    env.setdefault("ASAN_OPTIONS", "detect_leaks=0:abort_on_error=1")
    env.setdefault("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1")
    ppm = CACHE / "ppm"
    if args.sheet:
        ppm.mkdir(parents=True, exist_ok=True)
        env["BW_PPM"] = str(ppm)
    if args.heap_limit:
        env["BW_HEAP_LIMIT"] = str(args.heap_limit)
    result = subprocess.run([str(binary), args.source], cwd=ROOT, env=env)
    if args.sheet:
        sheet(ppm, args.sheet.resolve())
    raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
