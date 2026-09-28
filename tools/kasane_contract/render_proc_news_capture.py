"""Render four COM3 RGB565 capture logs as a side-by-side PNG (stdlib only)."""

import argparse
from pathlib import Path
import re
import struct
import zlib

W, H = 240, 135
PIX = re.compile(rb"^PIX (\d+) ([0-9a-f]{960})\r?$", re.M)


def rows(path: Path) -> list[bytes]:
    data = path.read_bytes()
    start = data.find(b"CAPTURE_BEGIN 240 135")
    if start < 0:
        raise ValueError(f"capture missing: {path}")
    found = {}
    for match in PIX.finditer(data, start):
        y = int(match.group(1))
        found.setdefault(y, match.group(2))
        if len(found) == H:
            break
    if set(found) != set(range(H)):
        raise ValueError(f"incomplete capture: {path}")
    result = []
    for y in range(H):
        rgb = bytearray()
        line = found[y]
        for x in range(W):
            pixel = int(line[x * 4 : x * 4 + 4], 16)
            rgb.extend(((pixel >> 11) * 255 // 31,
                        ((pixel >> 5) & 63) * 255 // 63,
                        (pixel & 31) * 255 // 31))
        result.append(bytes(rgb))
    return result


def chunk(kind: bytes, payload: bytes) -> bytes:
    return (struct.pack(">I", len(payload)) + kind + payload +
            struct.pack(">I", zlib.crc32(kind + payload)))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("logs", nargs="+", type=Path)
    args = parser.parse_args()
    frames = [rows(path) for path in args.logs]
    scanlines = b"".join(b"\0" + b"".join(frame[y] for frame in frames)
                         for y in range(H))
    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", struct.pack(">IIBBBBB", W * len(frames), H,
                                        8, 2, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(scanlines, 9)) +
           chunk(b"IEND", b""))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(png)
    print(f"{len(frames)} captures, {W * len(frames)}x{H}: {args.out}")


if __name__ == "__main__":
    main()
