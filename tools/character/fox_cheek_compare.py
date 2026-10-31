"""Reference art and fixed-camera unlit cheek proposals; standard library only."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib

import fox_asset_redesign as base
import fox_cheek_surfaces as proposed
import fox_surface_separation as prior

def read_reference(path):
 data=path.read_bytes();expected=json.loads((base.ROOT/'tools/character/fox_reference_landmarks.json').read_text())['sourceSha256']
 assert hashlib.sha256(data).hexdigest()==expected,'wrong reference image'
 assert data[:8]==b'\x89PNG\r\n\x1a\n';offset=8;compressed=bytearray();dimensions=None
 while offset<len(data):
  length=struct.unpack_from('>I',data,offset)[0];kind=data[offset+4:offset+8];payload=data[offset+8:offset+8+length]
  crc=struct.unpack_from('>I',data,offset+8+length)[0];assert zlib.crc32(kind+payload)&0xffffffff==crc
  if kind==b'IHDR':dimensions=struct.unpack('>IIBBBBB',payload)
  elif kind==b'IDAT':compressed.extend(payload)
  offset+=12+length
  if kind==b'IEND':break
 assert dimensions and dimensions[2:]==(8,2,0,0,0),'reference must be non-interlaced 8-bit RGB'
 w,h=dimensions[:2];stride=w*3;raw=zlib.decompress(compressed);assert len(raw)==h*(stride+1)
 result=bytearray();previous=bytearray(stride)
 for y in range(h):
  start=y*(stride+1);mode=raw[start];assert mode in range(5);row=bytearray(raw[start+1:start+1+stride])
  for i,value in enumerate(row):
   a=row[i-3] if i>=3 else 0;b=previous[i];c=previous[i-3] if i>=3 else 0
   if mode==1:predict=a
   elif mode==2:predict=b
   elif mode==3:predict=(a+b)//2
   elif mode==4:
    p=a+b-c;distances=[abs(p-a),abs(p-b),abs(p-c)];predict=(a,b,c)[distances.index(min(distances))]
   else:predict=0
   row[i]=(value+predict)&255
  result.extend(row);previous=row
 return w,h,result

def main():
 parser=argparse.ArgumentParser(description=__doc__)
 parser.add_argument('--reference',type=Path,help='optional original PNG; hash must match the recorded Library reference')
 parser.add_argument('--out',type=Path,default=base.ROOT/'.cache/fox-cheek-comparison.png')
 parser.add_argument('--face-crop',action='store_true',help='fixed 100x105 face region, nearest-neighbour 2x')
 args=parser.parse_args();sx,sy,sw,sh=(75,10,100,105) if args.face_crop else (0,0,240,135)
 column_width=sw*2;row_height=230 if args.face_crop else 285
 w,h=column_width*3,30+row_height*3;pixels=bytearray(bytes((24,28,32))*(w*h))
 def text(x,y,s):
  for c in s:
   for yy,bits in enumerate(base.glyph(c)):
    for xx in range(5):
     if bits&(1<<(4-xx)):
      p=((y+yy)*w+x+xx)*3;pixels[p:p+3]=b'\xe9\xee\xe8'
   x+=6
 text(5,5,'REFERENCE LIGHT/YAW UNCALIBRATED / OLD+NEW SAME UNLIT CAMERA / HOST')
 def panel(image,row,col,label):
  y0=30+row*row_height;x0=col*column_width;text(x0+5,y0-10,label)
  for y in range(sh):
   for x in range(sw):
    rgb=image[((y+sy)*240+x+sx)*3:((y+sy)*240+x+sx)*3+3]
    for dy in (0,1):
     for dx in (0,1):
      p=((y0+y*2+dy)*w+x0+x*2+dx)*3;pixels[p:p+3]=rgb
 text(5,20,'NO SOURCE VIEW / DO NOT MIRROR')
 if args.reference:
  rw,rh,reference=read_reference(args.reference);scale=66/172*1.3
  for col,cx,top,bounds,label in [(1,290,82,(190,0,390,200),'REFERENCE FRONT / ORIGINAL LIGHT'),(2,650,81,(550,0,740,200),'REFERENCE 3/4 / YAW UNKNOWN')]:
   image=bytearray(bytes(base.unpack(base.rgb565(base.BG)))*(240*135))
   for y in range(135):
    for x in range(240):
     sx=round((x-120)/scale+cx);sy=round((y-54.5)/scale+top)
     if bounds[0]<=sx<bounds[2] and bounds[1]<=sy<bounds[3] and 0<=sx<rw and 0<=sy<rh:
      image[(y*240+x)*3:(y*240+x)*3+3]=reference[(sy*rw+sx)*3:(sy*rw+sx)*3+3]
   panel(image,0,col,label)
 else:text(column_width+5,20,'REFERENCE NOT SUPPLIED')
 for row,(label,shapes) in enumerate([('OLD UNLIT',prior.new_author()),('NEW UNLIT',proposed.author())],start=1):
  for col,yaw in enumerate((-35,0,35)):panel(base.raster(shapes,yaw),row,col,label+f' YAW {yaw:+d}')
 args.out.parent.mkdir(parents=True,exist_ok=True);base.png(args.out,w,h,pixels);print(args.out)

if __name__=='__main__':main()
