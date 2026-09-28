"""Compare a USB RGB565 capture of the JS megademo with all host frames."""

import argparse
from pathlib import Path
import re


W, H = 240, 135
PIX = re.compile(rb"^PIX (\d+) ([0-9a-f]{960})\r?$", re.M)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--host", type=Path, required=True)
    args = parser.parse_args()

    log = args.log.read_bytes()
    records = re.findall(rb"PROC_JS: frame=(\d+) scalar=(\d+) pie=(\d+) bytes=(\d+)", log)
    backend_ok = bool(records) and all(
        int(scalar) == 0 and int(pie) == int(frame) + 1 and int(sent) == W * H * 2
        for frame, scalar, pie, sent in records
    )
    print(f"PIE present records: {len(records)}, valid={backend_ok}")
    captures = []
    rows = {}
    for match in PIX.finditer(log):
        row = int(match[1])
        if row == 0 and rows:
            captures.append(rows)
            rows = {}
        rows[row] = bytes.fromhex(match[2].decode("ascii"))
    if rows:
        captures.append(rows)
    complete = [c for c in captures if len(c) == H and set(c) == set(range(H))]
    if not complete:
        print(f"No complete capture: row counts {[len(c) for c in captures]}")
        return 2
    host = []
    for frame in range(48):
        path = args.host / f"frame-{frame:02d}.rgb565"
        pixels = path.read_bytes()
        if len(pixels) != W * H * 2:
            raise ValueError(f"Unexpected host frame size: {path}")
        host.append(pixels)
    all_match = True
    for index, rows in enumerate(complete):
        # PIX prints each RGB565 value in human-readable big-endian hex. Host
        # files store uint16_t as little-endian bytes, so swap each pair.
        capture = b"".join(
            bytes(value for i in range(0, W * 2, 2)
                  for value in (rows[y][i + 1], rows[y][i]))
            for y in range(H)
        )
        matches = [frame for frame, pixels in enumerate(host) if pixels == capture]
        print(f"Capture {index} matches host frame(s): {matches}")
        all_match &= bool(matches)
    return 0 if all_match and backend_ok else 2


if __name__ == "__main__":
    raise SystemExit(main())
