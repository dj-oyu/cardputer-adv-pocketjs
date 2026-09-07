"""Convert the host renderer's PPM files to PNG, with no image dependencies."""
from pathlib import Path
import struct
import zlib
import re

root = Path(__file__).resolve().parents[1]
font_header = root / 'build_flower/generated/fonts.h'
font = [int(v) for v in re.search(r'font_rows\[\]\s*=\s*\{([^}]+)', font_header.read_text()).group(1).replace('\n', '').split(',') if v.strip()]

def text(data, x, y, value, scale, color):
    # Same five-bit glyphs and six-pixel advance as the firmware's text().
    for letter in value:
        for gy in range(7):
            bits = font[(ord(letter)-32)*7+gy]
            for gx in range(5):
                if bits & (1 << (4-gx)):
                    for sy in range(scale):
                        for sx in range(scale):
                            p = ((y+gy*scale+sy)*240+x+gx*scale+sx)*3
                            data[p:p+3] = bytes(color)
        x += 6*scale

def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))

def label(data,x,y,value,scale,emphasis):
    text(data,x+1,y+1,value,scale,(2,7,15))
    text(data,x,y,value,scale,tuple(int(a+b*emphasis) for a,b in ((65,172),(100,146),(125,130))))

gallery=[]
for mode in ('ray', 'valley', 'sunflower', 'snowdrop','tulip','daffodil','crocus','calla'):
    path = Path(__file__).resolve().parents[1] / '.cache' / f'flower-{mode}.ppm'
    magic, size, maximum, data = path.read_bytes().split(b'\n', 3)
    assert magic == b'P6' and size == b'240 135' and maximum == b'255'
    assert len(data) == 240*135*3
    data = bytearray(data)
    # Gallery contains only the backgrounds, without any menu or title overlay.
    if mode in ('tulip','daffodil','crocus','calla'):gallery.append(bytearray(data))
    # Existing XMB, settled on Apps > Hello World. No flower-specific menu.
    label(data,16,37,'APPS',1,1)
    label(data,112,37,'SETTINGS',1,.35)
    label(data,16,69,'HELLO WORLD',2,1)
    label(data,16,89,'JAVASCRIPT / POCKETJS',1,.75)
    label(data,16,110,'SKK PRACTICE',2,.30)
    raw = b''.join(b'\0' + data[y*720:(y+1)*720] for y in range(135))
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 240, 135, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')
    path.with_suffix('.png').write_bytes(png)

# Four new procedural plants, enlarged with nearest-neighbour sampling.
rows=[]
for y in range(135):
    row=b''.join(bytes(p[y*720+120*3:(y+1)*720]) for p in gallery)
    row=b''.join(row[x:x+3]*3 for x in range(0,len(row),3))
    rows.extend([b'\0'+row]*3)
png=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',len(gallery)*360,405,8,2,0,0,0))
png+=chunk(b'IDAT',zlib.compress(b''.join(rows)))+chunk(b'IEND',b'')
(root/'.cache/flower-gallery.png').write_bytes(png)
