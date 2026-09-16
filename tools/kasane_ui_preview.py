"""Export renderer RGB565 smoke snapshots as PNG, without imaging dependencies."""
import pathlib
import struct
import sys
import zlib


def png(path, width, height, rgb):
    def chunk(kind, data):
        return (struct.pack('>I', len(data)) + kind + data
                + struct.pack('>I', zlib.crc32(kind + data)))
    raw = b''.join(b'\0' + rgb[y * width * 3:(y + 1) * width * 3]
                   for y in range(height))
    path.write_bytes(b'\x89PNG\r\n\x1a\n'
                     + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
                     + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


root = pathlib.Path(sys.argv[1])
width, height, gap = 240, 135, 12
sheet_w, sheet_h = width * 5 + gap * 6, height * 2 + gap * 3
sheet = bytearray(bytes((70, 75, 85)) * sheet_w * sheet_h)
for row, mode in enumerate(('native', 'js')):
    for step in range(5):
        src = root / f'{mode}-{step}.ppm'
        header, rgb = src.read_bytes().split(b'255\n', 1)
        assert header == b'P6\n240 135\n' and len(rgb) == width * height * 3
        png(src.with_suffix('.png'), width, height, rgb)
        x0, y0 = gap + step * (width + gap), gap + row * (height + gap)
        for y in range(height):
            start = ((y0 + y) * sheet_w + x0) * 3
            sheet[start:start + width * 3] = rgb[y * width * 3:(y + 1) * width * 3]
png(root / 'comparison.png', sheet_w, sheet_h, sheet)
print(root / 'comparison.png')
