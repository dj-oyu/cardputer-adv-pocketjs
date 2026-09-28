"""Make a contact sheet from test_flower_frame's fourteen fixed RGB565 shots."""
import argparse
import hashlib
import struct
import zlib
from pathlib import Path


def chunk(kind, payload):
    return (struct.pack('>I', len(payload)) + kind + payload +
            struct.pack('>I', zlib.crc32(kind + payload)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('shots', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    width, height, columns, count = 240, 135, 4, 14
    panel_bytes = width * height * 2
    frames = []
    hashes = []
    for index in range(count):
        raw = (args.shots / f'{index:02d}.rgb565').read_bytes()
        if len(raw) != panel_bytes:
            raise ValueError(f'species {index}: expected {panel_bytes} bytes, got {len(raw)}')
        frames.append(raw)
        hashes.append(hashlib.sha256(raw).hexdigest())
    if len(set(hashes)) != count:
        raise ValueError('two species have identical fixed shots')
    raster = bytearray()
    for y in range(height * 4):
        row = y // height
        source_y = y % height
        raster.append(0)  # PNG filter: none
        for x in range(width * columns):
            index = row * columns + x // width
            if index >= count:
                raster.extend((0, 0, 0))
                continue
            source_x = x % width
            offset = (source_y * width + source_x) * 2
            value = int.from_bytes(frames[index][offset:offset + 2], 'little')
            raster.extend((((value >> 11) & 31) * 255 // 31,
                           ((value >> 5) & 63) * 255 // 63,
                           (value & 31) * 255 // 31))
    png = (b'\x89PNG\r\n\x1a\n' +
           chunk(b'IHDR', struct.pack('>IIBBBBB', width * columns, height * 4,
                                      8, 2, 0, 0, 0)) +
           chunk(b'IDAT', zlib.compress(raster, 9)) + chunk(b'IEND', b''))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(png)
    print(f'{count} distinct species shots: {args.output}')


if __name__ == '__main__':
    main()
