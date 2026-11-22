#!/usr/bin/env python3
"""Pack opaque 240x135 PNG backdrops into one HCZ container for the Companion.

python pack_backgrounds.py OUTPUT.hcz "CHILD ROOM=child-room/background.png" ...

The raw image ("HBG1") is a 16-byte header, one 16-byte name per image, then the
images back to back as little-endian RGB565 (64,800 bytes each, top row first).
It is stored as word differences and deflated (HCZ2); main/hone/track_store.c unpacks it
into the ay_chars partition and the player composites rows straight from flash.
"""
import struct
import sys
import zlib
from pathlib import Path

from PIL import Image

WIDTH, HEIGHT = 240, 135


def rgb565(path):
    with Image.open(path) as im:
        if im.size != (WIDTH, HEIGHT):
            raise SystemExit(f"{path}: expected {WIDTH}x{HEIGHT}, got {im.size}")
        rgba = im.convert("RGBA")
        if rgba.getchannel("A").getextrema() != (255, 255):
            raise SystemExit(f"{path}: transparent pixels; export the composed opaque image")
        pixels = rgba.convert("RGB").tobytes()
    out = bytearray(WIDTH * HEIGHT * 2)
    for i in range(WIDTH * HEIGHT):
        r, g, b = pixels[3 * i:3 * i + 3]
        struct.pack_into("<H", out, 2 * i, ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
    return bytes(out)


def main(output, items):
    names, images = [], []
    for item in items:
        name, path = item.split("=", 1)
        if len(name.encode()) > 15:
            raise SystemExit(f"name too long: {name}")
        names.append(name)
        images.append(rgb565(path))
    header = struct.pack("<4sIHHI", b"HBG1", len(images), WIDTH, HEIGHT, 0)
    header += b"".join(n.encode().ljust(16, b"\0") for n in names)
    raw = header + b"".join(images)
    # Differences of successive words compress far better than the pixels themselves (HCZ2).
    words = struct.unpack(f"<{len(raw) // 2}H", raw)
    diff = struct.pack(f"<{len(words)}H", *((w - p) & 0xFFFF for w, p in zip(words, (0, *words))))
    packer = zlib.compressobj(9, zlib.DEFLATED, -15)
    packed = packer.compress(diff) + packer.flush()
    Path(output).write_bytes(struct.pack("<4sII", b"HCZ2", len(raw), zlib.crc32(raw)) + packed)
    print(f"{len(images)} backgrounds, raw {len(raw)} B, packed {len(packed) + 12} B")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    main(sys.argv[1], sys.argv[2:])
