"""Compare megademo device hashes and captured RGB565 rows with host frames."""

import argparse
from pathlib import Path
import re


W, H = 240, 135
MASK = (1 << 64) - 1


def host_pixels(path: Path) -> list[int]:
    data = path.read_bytes()
    header = b"P6\n240 135\n255\n"
    if not data.startswith(header) or len(data) != len(header) + W * H * 3:
        raise ValueError(f"invalid host frame {path}")
    rgb = data[len(header):]
    return [((rgb[i] >> 3) << 11) | ((rgb[i + 1] >> 2) << 5) | (rgb[i + 2] >> 3)
            for i in range(0, len(rgb), 3)]


def frame_hash(pixels: list[int]) -> int:
    h = 1469598103934665603
    for pixel in pixels:
        h = ((h ^ (pixel & 255)) * 1099511628211) & MASK
        h = ((h ^ (pixel >> 8)) * 1099511628211) & MASK
    return h


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", required=True, type=Path)
    parser.add_argument("--host", required=True, type=Path)
    args = parser.parse_args()
    host = [host_pixels(args.host / f"frame_{i:03}.ppm") for i in range(48)]
    expected = [frame_hash(pixels) for pixels in host]
    frame_re = re.compile(rb"KSN_MEGA: FRAME i=(\d+).*?hash=([0-9a-f]{16})")
    capture_re = re.compile(rb"KSN_MEGA: CAPTURE i=(\d+)")
    pixel_re = re.compile(rb"^PIX (\d+) ([0-9a-f]{960})$")
    found: dict[int, int] = {}
    captures: dict[int, dict[int, list[int]]] = {}
    active: int | None = None
    for line in args.log.read_bytes().splitlines():
        if match := capture_re.search(line):
            active = int(match[1])
            captures[active] = {}
        elif match := frame_re.search(line):
            found[int(match[1])] = int(match[2], 16)
            active = None
        elif active is not None and (match := pixel_re.match(line)):
            row = int(match[1])
            values = match[2]
            captures[active][row] = [int(values[x:x + 4], 16) for x in range(0, len(values), 4)]
    bad = [(i, f"{found.get(i, 0):016x}", f"{expected[i]:016x}")
           for i in range(48) if found.get(i) != expected[i]]
    print(f"Frame hashes: {48 - len(bad)}/48 match")
    for i, actual, want in bad[:8]:
        print(f"  frame {i}: device={actual} host={want}")
    capture_ok = True
    for i in (15, 31, 47):
        rows = captures.get(i, {})
        mismatches = sum(rows.get(y) != host[i][y * W:(y + 1) * W] for y in range(H))
        print(f"Capture {i}: {len(rows)}/{H} rows, {mismatches} mismatches")
        capture_ok &= not mismatches
    return 0 if not bad and capture_ok else 2


if __name__ == "__main__":
    raise SystemExit(main())
