"""Label completed native fox studies; preserve nearest-neighbour host pixels."""
import argparse
import json
from pathlib import Path
import struct
import sys
import zlib

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
from make_font import rows as glyph

def main():
 parser=argparse.ArgumentParser(description=__doc__)
 parser.add_argument('--lit-result',type=Path,required=True)
 parser.add_argument('--clay-result',type=Path,required=True)
 parser.add_argument('--old-lit-result',type=Path,help='optional prior geometry at the same camera/light')
 parser.add_argument('--out',type=Path,required=True)
 args=parser.parse_args()
 rows=[('LIT',args.lit_result),('CLAY',args.clay_result)]
 if args.old_lit_result:rows=[('OLD LIT',args.old_lit_result),('NEW LIT',args.lit_result),('NEW CLAY',args.clay_result)]
 w,h=1440,28+292*len(rows)
 pixels=bytearray(bytes((24,28,32))*(w*h))
 def text(x,y,s):
  for c in s:
   for yy,bits in enumerate(glyph(c)):
    for xx in range(5):
     if bits&(1<<(4-xx)):
      p=((y+yy)*w+x+xx)*3;pixels[p:p+3]=b'\xe9\xee\xe8'
   x+=6
 header='REAL QUICKJS / NATIVE RENDERER / HOST SIMULATION / NOT DEVICE CAPTURE OR FPS / SAME GEOMETRY, CAMERA AND WORLD LIGHT'
 if args.old_lit_result:header='REAL QUICKJS / NATIVE HOST / NOT DEVICE CAPTURE OR FPS / SAME CAMERA + WORLD LIGHT / GEOMETRY BY ROW'
 text(5,5,header)
 for row,(label,path) in enumerate(rows):
  result=json.loads(path.read_text());assert result['passed'] and not result['device_capture']
  folder=result['captures']
  if sys.platform=='win32' and folder.startswith('/mnt/c/'):folder='C:/'+folder[7:]
  folder=Path(folder)/'run-0'
  for col,yaw in enumerate((-35,0,35)):
   y0=28+row*292;x0=col*480;text(x0+5,y0,label+f' YAW {yaw:+d} / PIXELS 2X')
   data=(folder/f'app_{col:04}.ppm').read_bytes();header=b'P6\n240 135\n255\n'
   assert data.startswith(header) and len(data)==len(header)+240*135*3
   data=data[len(header):]
   for y in range(135):
    for x in range(240):
     rgb=data[(y*240+x)*3:(y*240+x)*3+3]
     for dy in (0,1):
      for dx in (0,1):
       p=((y0+12+y*2+dy)*w+x0+x*2+dx)*3;pixels[p:p+3]=rgb
 def chunk(t,b):return struct.pack('>I',len(b))+t+b+struct.pack('>I',zlib.crc32(t+b)&0xffffffff)
 raw=b''.join(b'\0'+pixels[y*w*3:(y+1)*w*3] for y in range(h))
 args.out.parent.mkdir(parents=True,exist_ok=True)
 args.out.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(raw,9))+chunk(b'IEND',b''))
 print(args.out)

if __name__=='__main__':main()
