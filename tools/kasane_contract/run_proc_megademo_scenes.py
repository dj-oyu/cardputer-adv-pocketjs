"""Differential oracle, limit checks and load statistics for every MEGADEMO scene.

Builds test_proc_megademo_scenes.c against the real QuickJS, the procedural
VM/plan and the Q14 point kernels with the PIE model, runs it over every tier
and every frame of apps/kasane/proc_megademo.js, and prints one statistics
line per tier and scene (MEGA_PLANS=1 adds one line per plan). With --out the
frames are also written as PPM and assembled into PNG contact sheets.
Host only: no device or serial port is used.
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
QJS = ROOT / "components/quickjs-ng/quickjs-ng"
OUT = ROOT / ".cache/kasane_megademo_scenes"
W, H = 240, 135
LENS = {0: 16, 1: 16, 2: 16, 3: 128, 4: 96, 5: 96}
GREY = bytes((0x30, 0x30, 0x30))


def compiler() -> str:
    for name in ("cc", "gcc", "clang"):
        found = shutil.which(name)
        if found:
            return found
    mingw = Path("C:/msys64/ucrt64/bin/gcc.exe")
    if mingw.exists():
        return str(mingw)
    raise SystemExit("No host C compiler found")


def build(opt: str) -> Path:
    OUT.mkdir(parents=True, exist_ok=True)
    cc = compiler()
    kasane = ROOT / "main/ui/kasane"
    binary = OUT / (f"test_proc_megademo_scenes_O{opt}" + (".exe" if sys.platform == "win32" else ""))
    sources = [*(QJS / n for n in ("dtoa.c", "libregexp.c", "libunicode.c", "quickjs.c", "quickjs-vm.c")),
               *(kasane / n for n in ("ksn_procedural.c", "ksn_proc_analysis.c", "ksn_proc_plan.c",
                                      "ksn_proc_plan_points.c", "ksn_proc_points.c",
                                      "ksn_proc_points_pie.c", "ksn_proc_points_dispatch.c")),
               ROOT / "tools/kasane_contract/test_proc_megademo_scenes.c"]
    command = [cc, "-std=gnu11", f"-O{opt}", "-DQUICKJS_NG_BUILD", "-D_GNU_SOURCE",
               "-DKSN_PROC_POINTS_PIE_MODEL", "-I", str(QJS), "-I", str(kasane),
               "-I", str(ROOT / "tools/kasane_contract"), *map(str, sources), "-lm", "-o", str(binary)]
    env = os.environ.copy()
    env["PATH"] = str(Path(cc).parent) + os.pathsep + env.get("PATH", "")
    subprocess.run(command, cwd=ROOT, env=env, check=True)
    return binary


def read_ppm(path: Path) -> bytes:
    data = path.read_bytes()
    header = b"P6\n240 135\n255\n"
    if not data.startswith(header) or len(data) != len(header) + W * H * 3:
        raise ValueError(f"unexpected PPM: {path}")
    return data[len(header):]


def frame_path(out: Path, tier: int, scene: int, frame: int) -> Path:
    for surface in (0, 1):
        p = out / f"t{tier}_s{scene}_f{frame:03}_{surface}.ppm"
        if p.exists():
            return p
    raise FileNotFoundError(f"tier {tier} scene {scene} frame {frame}")


def chunk(tag: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))


def sheet(path: Path, cells: list, columns: int) -> None:
    """cells: (tier, scene, frame) triples; a 2 px grey gap separates cells."""
    gap = 2
    rows = (len(cells) + columns - 1) // columns
    width, height = columns * W + (columns - 1) * gap, rows * H + (rows - 1) * gap
    images = [read_ppm(frame_path(path.parent, *c)) for c in cells]
    lines = bytearray()
    for y in range(height):
        lines.append(0)  # PNG filter type None.
        row, sy = divmod(y, H + gap)
        for column in range(columns):
            index = row * columns + column
            image = images[index] if index < len(images) else None
            if sy >= H or image is None:
                lines.extend(GREY * W)
            else:
                lines.extend(image[sy * W * 3:(sy + 1) * W * 3])
            if column + 1 < columns:
                lines.extend(GREY * gap)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(lines), 9)) + chunk(b"IEND", b"")
    path.write_bytes(png)


def sheets(out: Path, tier: int) -> None:
    # Per scene: 16 frames spread over the scene, left to right, top to
    # bottom. Radar (surface 1) frames appear where they fall.
    for scene, name in ((3, "twist"), (4, "zenith"), (5, "limit")):
        n = LENS[scene]
        picks = [round(i * (n - 1) / 15) for i in range(16)]
        sheet(out / f"sheet_{name}_tier{tier}.png", [(tier, scene, f) for f in picks], 4)
    # Act I: every other frame of the unchanged 48-frame reference.
    sheet(out / "sheet_act1.png", [(tier, s, f) for s in range(3) for f in range(0, 16, 2)], 4)
    # Tiers side by side (columns LIGHT, MID, HEAVY), one row per moment.
    cells = []
    for scene, frame in ((3, 64), (4, 30), (4, 52), (5, 20), (5, 60)):
        cells += [(t, scene, frame) for t in range(3)]
    sheet(out / "sheet_tiers.png", cells, 3)
    # Consecutive frames: the motion itself.
    sheet(out / "sheet_twist_motion.png", [(tier, 3, f) for f in range(56, 72)], 4)
    sheet(out / "sheet_zenith_motion.png", [(tier, 4, f) for f in range(40, 56)], 4)
    sheet(out / "sheet_limit_motion.png", [(tier, 5, f) for f in range(40, 56)], 4)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, help="write every frame as PPM and PNG sheets here")
    parser.add_argument("--opt", choices=("0", "2"), default="2")
    parser.add_argument("--source", type=Path, default=ROOT / "apps/kasane/proc_megademo.js")
    parser.add_argument("--sheet-tier", type=int, default=2, help="tier of the per-scene sheets")
    args = parser.parse_args()
    binary = build(args.opt)
    command = [str(binary), str(args.source.resolve())]
    if args.out:
        args.out.mkdir(parents=True, exist_ok=True)
        command.append(str(args.out.resolve()))
    subprocess.run(command, cwd=ROOT, check=True)
    if args.out:
        sheets(args.out.resolve(), args.sheet_tier)
        print(f"Wrote contact sheets to {args.out}")


if __name__ == "__main__":
    main()
