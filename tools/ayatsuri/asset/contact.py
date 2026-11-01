"""Exact-pixel synthetic native study contact sheet; standard-library only."""
from __future__ import annotations
import argparse
from pathlib import Path
import struct
import zlib

FONT = {
 ' ': ['000','000','000','000','000'], '-':['000','000','111','000','000'], '+':['000','010','111','010','000'], '.':['000','000','000','000','010'], '/':['001','001','010','100','100'],
 '0':['111','101','101','101','111'], '1':['010','110','010','010','111'], '2':['110','001','010','100','111'], '3':['110','001','010','001','110'], '4':['101','101','111','001','001'], '5':['111','100','110','001','110'], '6':['011','100','111','101','111'], '7':['111','001','010','010','010'], '8':['111','101','111','101','111'], '9':['111','101','111','001','110'],
 'A':['010','101','111','101','101'], 'B':['110','101','110','101','110'], 'C':['011','100','100','100','011'], 'D':['110','101','101','101','110'], 'E':['111','100','110','100','111'], 'F':['111','100','110','100','100'], 'G':['011','100','101','101','011'], 'H':['101','101','111','101','101'], 'I':['111','010','010','010','111'], 'L':['100','100','100','100','111'], 'M':['101','111','111','101','101'], 'N':['101','111','111','111','101'], 'O':['010','101','101','101','010'], 'P':['110','101','110','100','100'], 'R':['110','101','110','101','101'], 'S':['011','100','010','001','110'], 'T':['111','010','010','010','010'], 'U':['101','101','101','101','111'], 'V':['101','101','101','101','010'], 'W':['101','101','111','111','101'], 'X':['101','101','010','101','101'], 'Y':['101','101','010','010','010']}


def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)


def read_png(path):
    data = path.read_bytes()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('PNG signature')
    offset, compressed = 8, bytearray()
    while offset < len(data):
        length = struct.unpack_from('>I', data, offset)[0]
        kind, payload = data[offset + 4:offset + 8], data[offset + 8:offset + 8 + length]
        if offset + length + 12 > len(data) or zlib.crc32(kind + payload) & 0xffffffff != struct.unpack_from('>I', data, offset + 8 + length)[0]:
            raise ValueError('PNG chunk')
        if kind == b'IHDR' and payload != struct.pack('>IIBBBBB', 240, 135, 8, 2, 0, 0, 0):
            raise ValueError('expected 240x135 RGB8 oracle output')
        if kind == b'IDAT':
            compressed.extend(payload)
        offset += length + 12
    raw = zlib.decompress(bytes(compressed))
    if len(raw) != 135 * 721 or any(raw[y * 721] != 0 for y in range(135)):
        raise ValueError('expected unfiltered oracle rows')
    return b''.join(raw[y * 721 + 1:(y + 1) * 721] for y in range(135))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('study', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    width, height = 1240, 486
    rgb = bytearray([18, 22, 30] * (width * height))
    def text(x, y, value):
        for char in value:
            for dy, row in enumerate(FONT[char]):
                for dx, bit in enumerate(row):
                    if bit == '1':
                        at = ((y + dy) * width + x + dx) * 3
                        rgb[at:at + 3] = bytes((224, 230, 240))
            x += 4
    text(8, 7, 'SYNTHETIC HOST / RGB565 / NOT LCD')
    for i, title in enumerate(['SOURCE', 'NATIVE 49V', 'DEPTH 49V', 'NATIVE 65V', 'DEPTH 65V']):
        text(30 + i * 242, 19, title)
    for row, (suffix, label) in enumerate([('front', 'FRONT'), ('yaw-35', '-35'), ('yaw35', '+35')]):
        y = 30 + row * 145
        text(2, y + 63, label)
        for col, (case, image) in enumerate([(f'v65-{suffix}', 'source'), (f'v49-{suffix}', 'native'), (f'v49-{suffix}', 'depth'), (f'v65-{suffix}', 'native'), (f'v65-{suffix}', 'depth')]):
            pixels = read_png(args.study / (case + '-oracle') / (image + '.png'))
            x = 30 + col * 242
            for dy in range(135):
                at = ((y + dy) * width + x) * 3
                rgb[at:at + 720] = pixels[dy * 720:(dy + 1) * 720]
    text(8, 471, 'DEPTH GREEN 0 / RED .01 SOURCE M / 49V FAILS YAW / 65V PASSES SAMPLED VIEWS')
    raw = b''.join(b'\0' + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


if __name__ == '__main__':
    main()
