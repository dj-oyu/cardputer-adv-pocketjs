"""Side-by-side host preview of the crowd variants priced in
docs/apps/derby-pan-camera-cost.md ("観客をノイズにする案"). Draws the stands'
crowd of a MID WIDE side view four ways on 240 x 40 strips, stacked, scaled
x3, as one PNG. The look is not judged here; this only hands the pictures over.

  today   the crowd plan (the hand IR's T.crowd, tools/kasane_ir/derby_hand_ir.js) run in float32
  tile    3 random-walk polyline tiles (typed points), chosen per bay by hash
  quarter today's dots, every 4th kept
  bands   2 full-width lines per row

  python tools/games/pancost/crowd_preview.py --out .cache/pancost/crowd.png
"""
from __future__ import annotations

import argparse
import math
import struct
import zlib
from pathlib import Path

W, H, S = 240, 40, 3
BG, STAND = (0x18, 0x24, 0x1c), (0x33, 0x40, 0x3a)


def rgb565(c):
    return ((c >> 11 & 31) * 255 // 31, (c >> 5 & 63) * 255 // 63, (c & 31) * 255 // 31)


def f32(x):
    return struct.unpack("f", struct.pack("f", x))[0]


def crowd_today(blink=0, rows=3, dots=4, a=-5.0, dx=30.0, gy=30.0, rs=-6.0, n=9, phase=0.0, keep=1):
    """T.crowd, step by step (MID: 3 rows, 4 dots a bay), float32."""
    out = []
    c = f32(blink * 12650 + 46496)
    step = f32(dx * f32(1 / dots))
    y = f32(gy + f32(rs * 0.5))
    r13 = 0.0
    k = 0
    for _ in range(rows):
        x = f32(a)
        ph = f32(phase + r13)
        for _ in range(n):
            for _ in range(dots):
                px = f32(f32(f32(math.sin(ph)) * 1.5) + x)
                if (k * 2654435761 >> 13) % keep == 0:
                    out.append((round(px), round(y), int(c)))
                k += 1
                x = f32(x + step)
                ph = f32(ph + 2.39996)
                c = f32(f32(-c) + 105642)
        y = f32(y + rs)
        r13 = f32(r13 + 0.9)
    return out


def tiles(n=9, a=-5, dx=30, gy=30, pts=12):
    """Random-walk polylines (the typed-points 'noise'), 3 tile types."""
    types = []
    for seed in (0x9e3779b9, 0x7f4a7c15, 0x2545f491):
        h, u, v, t = seed, dx // 2, 8, []
        for _ in range(pts):
            h = (h ^ h << 13) & 0xffffffff; h ^= h >> 17; h = (h ^ h << 5) & 0xffffffff
            u = max(0, min(dx - 2, u + (h & 15) - 7))
            v = max(0, min(15, v + (h >> 4 & 7) - 3))
            t.append((u, v))
        types.append(t)
    segs = []
    for j in range(n):
        t = types[((j + 11) * 2654435761 >> 16) % 3]
        ox, oy = a + j * dx, gy - 16
        for (x0, y0), (x1, y1) in zip(t, t[1:]):
            segs.append((ox + x0, oy + y0, ox + x1, oy + y1, 46496 if j & 1 else 59146))
    return segs


def bands(rows=3, gy=30, rs=-6):
    return [(0, round(gy + rs * (r + .5)) + d, W - 1, round(gy + rs * (r + .5)) + d, 46496 if r & 1 else 59146)
            for r in range(rows) for d in (0, 1)]


def line(img, x0, y0, x1, y1, col):
    dx, dy = abs(x1 - x0), -abs(y1 - y0)
    sx, sy = (1 if x0 < x1 else -1), (1 if y0 < y1 else -1)
    err = dx + dy
    while True:
        if 0 <= x0 < W and 0 <= y0 < H:
            img[y0][x0] = col
        if x0 == x1 and y0 == y1:
            return
        e2 = 2 * err
        if e2 >= dy:
            err += dy; x0 += sx
        if e2 <= dx:
            err += dx; y0 += sy


def panel(dots=(), segs=()):
    img = [[BG] * W for _ in range(H)]
    for y in range(8, 32):
        img[y] = [STAND] * W
    for x, y, c in dots:
        if 0 <= x < W and 0 <= y < H:
            img[y][x] = rgb565(c)
    for x0, y0, x1, y1, c in segs:
        line(img, x0, y0, x1, y1, rgb565(c))
    return img


def png(path, rows):
    raw = b"".join(b"\0" + bytes(v for px in r for v in px) for r in rows)
    chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", len(rows[0]), len(rows), 8, 2, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=".cache/pancost/crowd.png")
    a = ap.parse_args()
    panels = [panel(dots=crowd_today()), panel(segs=tiles()), panel(dots=crowd_today(keep=4)), panel(segs=bands())]
    sep = [[(255, 255, 255)] * W]
    rows = []
    for p in panels:
        rows += p + sep
    big = [[px for px in r for _ in range(S)] for r in rows for _ in range(S)]
    png(a.out, big)
    print("wrote", a.out, "panels: today, tile, quarter, bands")


if __name__ == "__main__":
    main()
