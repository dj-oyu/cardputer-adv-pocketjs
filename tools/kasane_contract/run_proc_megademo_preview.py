"""Compile and render the procedural megademo using the host Kasane IR path.

Run from any directory: python tools/kasane_contract/run_proc_megademo_preview.py
Outputs 48 RGB PPMs, a PNG contact sheet, and frame diagnostics under
.cache/kasane_megademo_preview. No device or serial connection is used.
"""

from __future__ import annotations

import argparse
import os
import shutil
import struct
import subprocess
import sys
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_OUT = ROOT / ".cache" / "kasane_megademo_preview"
W, H = 240, 135


def compiler() -> str:
    for name in ("cc", "gcc", "clang"):
        found = shutil.which(name)
        if found:
            return found
    mingw = Path("C:/msys64/ucrt64/bin/gcc.exe")
    if mingw.exists():
        return str(mingw)
    raise SystemExit("No host C compiler found (tried cc, gcc, clang, MSYS2 UCRT64 gcc)")


def read_ppm(path: Path) -> bytes:
    with path.open("rb") as f:
        if f.readline() != b"P6\n" or f.readline() != b"240 135\n" or f.readline() != b"255\n":
            raise ValueError(f"unexpected PPM header: {path}")
        data = f.read()
    if len(data) != W * H * 3:
        raise ValueError(f"unexpected PPM size: {path}")
    return data


def chunk(tag: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))


def contact_sheet(out: Path) -> None:
    # Every other frame preserves the full three-phase timeline in a compact image.
    images = [read_ppm(out / f"frame_{tick:03}.ppm") for tick in range(0, 48, 2)]
    columns, rows = 4, 6
    width, height = columns * W, rows * H
    scanlines = bytearray()
    for y in range(height):
        row, source_y = divmod(y, H)
        scanlines.append(0)  # PNG filter type None.
        for column in range(columns):
            image = images[row * columns + column]
            start = source_y * W * 3
            scanlines.extend(image[start:start + W * 3])
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(scanlines, level=9))
    png += chunk(b"IEND", b"")
    (out / "contact_sheet.png").write_bytes(png)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    binary = out / ("proc_megademo_preview.exe" if sys.platform == "win32" else "proc_megademo_preview")
    cmd = [
        compiler(), "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
        "-I", str(ROOT / "main/ui/kasane"),
        "-I", str(ROOT / "tools/kasane_contract"),
        str(ROOT / "main/ui/kasane/ksn_procedural.c"),
        str(ROOT / "main/ui/kasane/ksn_proc_analysis.c"),
        str(ROOT / "main/ui/kasane/ksn_proc_plan.c"),
        str(ROOT / "tools/kasane_contract/proc_megademo_preview.c"),
        "-lm", "-o", str(binary),
    ]
    env = os.environ.copy()
    # MSYS2's compiler children (cc1, assembler, linker) need its DLL directory.
    env["PATH"] = str(Path(cmd[0]).parent) + os.pathsep + env.get("PATH", "")
    subprocess.run(cmd, check=True, cwd=ROOT, env=env)
    with (out / "diagnostics.txt").open("w", encoding="utf-8") as log:
        subprocess.run([str(binary), str(out)], check=True, cwd=ROOT, stdout=log)
    contact_sheet(out)
    print(f"Rendered 48 PPM frames and {out / 'contact_sheet.png'}")


if __name__ == "__main__":
    main()
