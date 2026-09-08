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
# Share the host test's catalog so adding a species cannot silently omit it.
modes=re.findall(r'"([a-z]+)"', (root/'tools/flower_catalog.h').read_text())
species_enum=re.search(r'typedef enum\s*\{(.*?)\}', (root/'main/scene/flower.h').read_text(),re.S).group(1)
source_names=[name.lower() for name in re.findall(r'FLOWER_([A-Z]+)\s*,',species_enum)]
assert modes==source_names, 'preview names must match the source enum in order'
for mode in modes:
    path = Path(__file__).resolve().parents[1] / '.cache' / f'flower-{mode}.ppm'
    magic, size, maximum, data = path.read_bytes().split(b'\n', 3)
    assert magic == b'P6' and size == b'240 135' and maximum == b'255'
    assert len(data) == 240*135*3
    data = bytearray(data)
    # Gallery contains only the backgrounds, without any menu or title overlay.
    tile=bytearray(data)
    text(tile,124,3,'FLOWER_'+mode.upper(),1,(230,240,245))
    gallery.append(tile)
    # A one-to-one review image, labelled with the exact C enum identifier.
    enlarged=[]
    for y in range(135):
        row=tile[y*720+360:(y+1)*720]
        row=b''.join(row[x:x+3]*3 for x in range(0,len(row),3))
        enlarged.extend([b'\0'+row]*3)
    model=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',360,405,8,2,0,0,0))
    model+=chunk(b'IDAT',zlib.compress(b''.join(enlarged)))+chunk(b'IEND',b'')
    (root/'.cache'/f'flower-model-{mode}.png').write_bytes(model)
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

# All botanicals in four columns, enlarged with nearest-neighbour sampling.
rows=[]
height=((len(gallery)+3)//4)*405
gallery.extend([bytearray(240*135*3)]*((-len(gallery))%4))
for start in range(0,len(gallery),4):
    for y in range(135):
        row=b''.join(bytes(p[y*720+120*3:(y+1)*720]) for p in gallery[start:start+4])
        row=b''.join(row[x:x+3]*3 for x in range(0,len(row),3))
        rows.extend([b'\0'+row]*3)
png=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',1440,height,8,2,0,0,0))
png+=chunk(b'IDAT',zlib.compress(b''.join(rows)))+chunk(b'IEND',b'')
(root/'.cache/flower-gallery.png').write_bytes(png)
