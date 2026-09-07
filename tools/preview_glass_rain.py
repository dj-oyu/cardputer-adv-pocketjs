"""Make an animated PNG with the existing XMB above the glass effect."""
from pathlib import Path
import struct
import zlib
from preview_flower import label, chunk

root=Path(__file__).resolve().parents[1]
png=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',240,135,8,2,0,0,0))
png+=chunk(b'acTL',struct.pack('>II',45,0))
sequence=0
for i in range(45):
    data=bytearray((root/'.cache'/f'rain-{i:02d}.ppm').read_bytes().split(b'\n',3)[3])
    assert len(data)==240*135*3
    label(data,16,37,'APPS',1,1)
    label(data,112,37,'SETTINGS',1,.35)
    label(data,16,69,'HELLO WORLD',2,1)
    label(data,16,89,'JAVASCRIPT / POCKETJS',1,.75)
    label(data,16,110,'SKK PRACTICE',2,.3)
    png+=chunk(b'fcTL',struct.pack('>IIIIIHHBB',sequence,240,135,0,0,1,15,0,0));sequence+=1
    raw=b''.join(b'\0'+data[y*720:(y+1)*720] for y in range(135))
    compressed=zlib.compress(raw)
    if i==0:png+=chunk(b'IDAT',compressed)
    else:
        png+=chunk(b'fdAT',struct.pack('>I',sequence)+compressed);sequence+=1
png+=chunk(b'IEND',b'')
(root/'.cache/flower-rain.png').write_bytes(png)
print('RAIN_PREVIEW 45 frames, 3-second host animation (not measured device FPS)')
