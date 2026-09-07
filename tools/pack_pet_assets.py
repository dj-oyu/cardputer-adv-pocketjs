"""Compile the approved concept sheet into twelve 64px ABGR4444 textures.

Pillow is a host-only dependency. The device needs no PNG decoder, source
pixels in the JS heap, or simultaneous allocation of twelve textures.
"""
from pathlib import Path
from collections import deque
import struct
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / 'apps/pet/assets'
sheet = Image.open(ASSETS / 'concepts.png').convert('RGB')
# Each cell includes the full tail/crest and excludes the neighboring row.
xs = [70, 425, 765, 1110, 1450]
ys = [35, 382, 648, 980]
packed = bytearray()
for row in range(3):
    for col in range(4):
        cell = sheet.crop((xs[col], ys[row], xs[col+1], ys[row+1]))
        w, h = cell.size
        pixels = cell.load()
        outside = set()
        todo = deque([(x, y) for x in range(w) for y in (0, h-1)] +
                     [(x, y) for y in range(h) for x in (0, w-1)])
        while todo:
            x, y = todo.popleft()
            if not (0 <= x < w and 0 <= y < h) or (x, y) in outside:
                continue
            r, g, b = pixels[x, y]
            if min(r, g, b) < 215 or max(r, g, b)-min(r, g, b) > 42:
                continue
            outside.add((x, y))
            todo.extend(((x-1, y), (x+1, y), (x, y-1), (x, y+1)))
        rgba = cell.convert('RGBA')
        for xy in outside:
            rgba.putpixel(xy, (0, 0, 0, 0))
        rgba = rgba.crop(rgba.getbbox())
        rgba.thumbnail((60, 60), Image.Resampling.NEAREST)
        tile = Image.new('RGBA', (64, 64))
        tile.paste(rgba, ((64-rgba.width)//2, 62-rgba.height))
        for r, g, b, a in tile.getdata():
            packed.extend(struct.pack('<H', (r>>4) | ((g>>4)<<4) |
                                      ((b>>4)<<8) | ((a>>4)<<12)))
(ASSETS / 'pets.bin').write_bytes(packed)
print(f'12 textures, {len(packed)} flash bytes, 8192 active texture bytes')
