"""One offline fox proposal: restrained muzzle and broad curved cheek fur."""
import hashlib
import argparse
import math
from pathlib import Path
import sys
import struct

import fox_asset_redesign as base
import fox_surface_separation as separated

MUZZLE=dict(center=[0,7.5],support=[9,5.5],amplitude=9)
# Each station gives a head-attached root and a tapered outer contour.
STATIONS=[dict(root=[17,-.5],outer=[25,0]),dict(root=[18,3],outer=[32,4]),
          dict(root=[17,6.5],outer=[29.5,6.7]),dict(root=[15.5,10],outer=[31,9.5]),
          dict(root=[12,12.5],outer=[17,13])]

def surface(x,y):return separated.surface(x,y,muzzle=MUZZLE)

def head_sample(face,x,y):
 # Match the immutable F32 head mesh, not its unsampled analytic source field.
 f32=lambda value:struct.unpack('<f',struct.pack('<f',value))[0]
 points=[[f32(v) for v in point] for point in face['vertices']]
 found=[]
 for ids in face['triangles']:
  a,b,c=[points[i] for i in ids]
  ux,uy=b[0]-a[0],b[1]-a[1];vx,vy=c[0]-a[0],c[1]-a[1];det=ux*vy-uy*vx
  u=((x-a[0])*vy-(y-a[1])*vx)/det;v=(ux*(y-a[1])-uy*(x-a[0]))/det
  if u>=-1e-7 and v>=-1e-7 and u+v<=1+1e-7:
   weights=(1-u-v,u,v);z=sum(w*points[i][2] for w,i in zip(weights,ids))
   normal=[sum(w*f32(face['normals'][i][j]) for w,i in zip(weights,ids)) for j in range(3)]
   length=math.hypot(*normal);normal=[n/length for n in normal]
   mask=sum(w*f32(face['mask'][i]) for w,i in zip(weights,ids))
   found.append((z,normal,mask))
 assert found,'cheek root must be inside the final head mesh'
 assert all(abs(item[0]-found[0][0])<1e-5 for item in found),'shared head depth must agree'
 return found[0]

def cheek(side,face,attach_roots):
 vertices=[];triangles=[]
 for row,station in enumerate(STATIONS):
  x0,y0=station['root'];x1,y1=station['outer'];v=row/(len(STATIONS)-1)
  z0=head_sample(face,side*x0,y0)[0]+.05 if attach_roots else surface(x0,y0)+.2
  z1=(surface(x1,y1)+.2) if row in (0,len(STATIONS)-1) else [None,1,2.5,.5,None][row]
  for column in range(4):
   u=column/3;x=x0+(x1-x0)*u;y=y0+(y1-y0)*u
   # A curved sheet with width; the rim is not a stack of thin triangle tips.
   z=z0+(z1-z0)*u+3*math.sin(math.pi*u)*math.sin(math.pi*v)
   vertices.append([side*x,y,z])
 for row in range(len(STATIONS)-1):
  for column in range(3):
   a=row*4+column;b=a+1;c=a+4;d=c+1
   triangles.extend(((a,c,d),(a,d,b)))
 triangles=base.front_winding(vertices,triangles)
 normals=base.normals(vertices,triangles)
 masks=[y-base.interpolate(base.BOUNDARY,abs(x)) for x,y,z in vertices]
 if attach_roots:
  for row in range(len(STATIONS)):
   at=row*4;x,y,z=vertices[at];depth,normal,mask=head_sample(face,x,y)
   assert abs(z-depth-.05)<1e-7
   normals[at]=normal;masks[at]=mask
 return dict(name=f'cheek-fur-surface-{side}',vertices=vertices,triangles=triangles,
  normals=normals,mask=masks,
  a=base.ORANGE,b=base.CREAM)

def author(attach_roots=True):
 original=separated.new_author(surface_fn=surface)
 result=[original[0],cheek(-1,original[0],attach_roots),cheek(1,original[0],attach_roots)]+[s for s in original[1:] if not s['name'].startswith('cheek-fur-')]
 assert all(separated.muzzle_offset(x,y,MUZZLE)==0 for x in (-33,-12,-9,9,12,33) for y in (-25,2,7.5,13,17))
 assert separated.muzzle_offset(0,7.5,MUZZLE)==9
 assert all(station['root'][0]>MUZZLE['support'][0] for station in STATIONS)
 assert all(len(s['vertices'])==20 and len(s['triangles'])==24 for s in result[1:3])
 return result

if __name__=='__main__':
 parser=argparse.ArgumentParser(description=__doc__)
 parser.add_argument('--out-dir',type=Path,default=base.ROOT/'.cache/fox-cheek-joined')
 parser.add_argument('--root-attachment',choices=('mesh','analytic'),default='mesh')
 args=parser.parse_args();joined=args.root_attachment=='mesh'
 config=dict(mode='restrained-muzzle-curved-cheek-sheets',skull=separated.SKULL,muzzle=MUZZLE,
  cheekStations=STATIONS,crossSamples=4,curveHeight=3,rootAttachment=args.root_attachment,
  sourceSha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
  notes=['Skull and forelock topology are retained from the separated design.',
   'Muzzle support and amplitude are reduced independently of the skull.',
   'Two broad curved sheets replace six thin cheek triangles.'])
 base.main(['--out-dir',str(args.out_dir)],authoring=lambda:author(joined),authoring_config=config,surface_fn=surface)
