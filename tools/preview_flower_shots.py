"""Contact sheets for the FLOWER shot list: one PNG per shot, every species.

Reads the PPMs written by tools/preview_flower_shots.c. No image dependencies,
and it borrows nothing from preview_flower.py except the idea; the two write
disjoint files so they cannot collide.
"""
from pathlib import Path
import sys
import struct
import zlib
import re

root = Path(__file__).resolve().parents[1]
cache = root / '.cache'
variant = sys.argv[1] if len(sys.argv) > 1 else 'live'
ppms = cache / f'var-{variant}'
header = root / ('.cache/variants/' + ('live' if variant.startswith('--') else variant) + '/flower_shots.h')

font = [int(v) for v in re.search(r'font_rows\[\]\s*=\s*\{([^}]+)',
        (root / 'build_flower/generated/fonts.h').read_text()
        ).group(1).replace('\n', '').split(',') if v.strip()]

names = re.findall(r'"([a-z]+)"', (root / 'tools/flower_catalog.h').read_text())
shots = re.findall(r'\{\s*([-+][\d.]+)f,\s*([\d.]+)f,\s*([\d.]+)f,\s*([\d.]+)f\s*\}',
                   header.read_text())
assert shots, 'could not parse FLOWER_SHOTS'


def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))


def png(path, width, height, rows):
    blob = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
    blob += chunk(b'IDAT', zlib.compress(b''.join(rows))) + chunk(b'IEND', b'')
    path.write_bytes(blob)


def text(buf, stride, x, y, value, color):
    for letter in value.upper():
        for gy in range(7):
            bits = font[(ord(letter) - 32) * 7 + gy]
            for gx in range(5):
                if bits & (1 << (4 - gx)):
                    p = ((y + gy) * stride + x + gx) * 3
                    buf[p:p + 3] = bytes(color)
        x += 6


def load(shot, name):
    path = ppms / f'shot-{shot}-{name}.ppm'
    magic, size, maximum, data = path.read_bytes().split(b'\n', 3)
    assert magic == b'P6' and size == b'240 135' and maximum == b'255'
    tile = bytearray(data)
    text(tile, 240, 124, 124, name, (235, 245, 250))
    return tile


COLS = 4
W, H = 240, 135
for index, (pitch, zoom, aim, hold) in enumerate([] if variant.startswith('--') else shots):
    tiles = [load(index, n) for n in names]
    tiles += [bytearray(W * H * 3)] * ((-len(tiles)) % COLS)
    rows = []
    banner = bytearray(W * COLS * 12 * 3)
    text(banner, W * COLS, 6, 3,
         f'{variant}  shot {index}  zoom {zoom}  pitch {pitch}  aim {aim}  hold {hold}s',
         (255, 220, 150))
    for y in range(12):
        rows.append(b'\0' + bytes(banner[y * W * COLS * 3:(y + 1) * W * COLS * 3]))
    for start in range(0, len(tiles), COLS):
        for y in range(H):
            rows.append(b'\0' + b''.join(
                bytes(t[y * W * 3:(y + 1) * W * 3]) for t in tiles[start:start + COLS]))
    png(cache / f'flower-{variant}-shot-{index}.png', W * COLS, 12 + H * (len(tiles) // COLS), rows)
    print(f'.cache/flower-{variant}-shot-{index}.png')

# One ladder sheet: the shots down the page for a handful of species across it,
# which is the view that shows whether consecutive cuts differ in SIZE.
LADDER = ['valley', 'sunflower', 'iris', 'echinacea']
rows = []
for index, shot in enumerate([] if variant.startswith('--') else shots):
    tiles = [load(index, n) for n in LADDER]
    for y in range(H):
        rows.append(b'\0' + b''.join(
            bytes(t[y * W * 3:(y + 1) * W * 3]) for t in tiles))
if not variant.startswith('--'):
    png(cache / f'flower-{variant}-ladder.png', W * len(LADDER), H * len(shots), rows)
    print(f'.cache/flower-{variant}-ladder.png')


def compare_stack(shot, variants, out):
    """One shot, several variants, stacked. The comparison the per-variant
    sheets cannot show: the same cut on one page rather than pages apart."""
    global ppms
    rows = []
    for name in variants:
        ppms = cache / f'var-{name}'
        tiles = [load(shot, n) for n in LADDER]
        banner = bytearray(W * len(LADDER) * 12 * 3)
        text(banner, W * len(LADDER), 6, 3, f'shot {shot}   {name}', (255, 220, 150))
        for y in range(12):
            rows.append(b'\0' + bytes(banner[y * W * len(LADDER) * 3:(y + 1) * W * len(LADDER) * 3]))
        for y in range(H):
            rows.append(b'\0' + b''.join(bytes(t[y * W * 3:(y + 1) * W * 3]) for t in tiles))
    png(cache / out, W * len(LADDER), len(variants) * (12 + H), rows)
    print(f'.cache/{out}')


if variant == '--compare':
    # The fitted wide against the constant it replaces: the fit is per species,
    # so the difference is a different amount on every plant.
    compare_stack(0, ['live', 'constant-wide'], 'flower-ab-fit.png')
    # The framings, widest to tightest, on one page.
    compare_stack(0, ['live', 'middle', 'tight'], 'flower-framings.png')
    # Pitch, which costs nothing either way.
    compare_stack(0, ['live', 'low'], 'flower-ab-pitch.png')


def sheet2x(name, shot, out, cols=3):
    """One framing, every species, doubled. Judging whether a part leaving the
    window reads as cropped or as framed needs the pixels big enough to see."""
    global ppms
    ppms = cache / ("var-" + name)
    tiles = [load(shot, n) for n in names]
    tiles += [bytearray(W * H * 3)] * ((-len(tiles)) % cols)
    rows = []
    for start in range(0, len(tiles), cols):
        for y in range(H):
            row = b"".join(bytes(t[y * W * 3:(y + 1) * W * 3]) for t in tiles[start:start + cols])
            row = b"".join(row[x:x + 3] * 2 for x in range(0, len(row), 3))
            rows.extend([chr(0).encode() + row] * 2)
    png(cache / out, W * cols * 2, H * 2 * (len(tiles) // cols), rows)
    print(".cache/" + out)


if variant == "--tight":
    sheet2x("tight", 0, "flower-tight-2x.png")
    sheet2x("live", 0, "flower-fit-2x.png")
