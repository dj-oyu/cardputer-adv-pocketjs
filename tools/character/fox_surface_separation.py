"""Separate smooth skull, compact muzzle deformation and contour fur offline."""
import hashlib
import math
from pathlib import Path
import sys

import fox_asset_redesign as base

SKULL=dict(center=[0,-4,-6],radii=[27,21,18])
MUZZLE=dict(center=[0,8],support=[11,8],amplitude=14)

def muzzle_offset(x,y):
 # Compact C1 support: beyond this ellipse neither displacement nor slope leaks.
 s=((x-MUZZLE['center'][0])/MUZZLE['support'][0])**2+((y-MUZZLE['center'][1])/MUZZLE['support'][1])**2
 return MUZZLE['amplitude']*max(0,1-s)**2

def surface(x,y):
 cx,cy,cz=SKULL['center'];rx,ry,rz=SKULL['radii']
 q=1-((x-cx)/rx)**2-((y-cy)/ry)**2
 return cz+rz*math.sqrt(max(0,q))+muzzle_offset(x,y)

def new_author():
 vertices=[];rings=[];triangles=[]
 # Angular sampling preserves crown curvature; fur tips are not skull vertices.
 for degrees in range(0,181,18):
  p=math.radians(degrees);y=SKULL['center'][1]-SKULL['radii'][1]*math.cos(p);width=SKULL['radii'][0]*math.sin(p)
  if degrees in (0,180):width=0
  ring=[]
  for t in ([0] if width==0 else [-1,-.75,-.5,-.25,0,.25,.5,.75,1]):
   x=SKULL['center'][0]+width*t;ring.append(len(vertices));vertices.append([x,y,surface(x,y)])
  rings.append(ring)
 for a,b in zip(rings,rings[1:]):
  if len(a)==1:triangles.extend((a[0],b[i],b[i+1]) for i in range(8))
  elif len(b)==1:triangles.extend((a[i],b[0],a[i+1]) for i in range(8))
  else:
   for i in range(8):triangles.extend(((a[i],b[i],b[i+1]),(a[i],b[i+1],a[i+1])))
 triangles=base.front_winding(vertices,triangles)
 face=dict(name='continuous-face',vertices=vertices,triangles=triangles,normals=base.normals(vertices,triangles),
           mask=[y-base.interpolate(base.BOUNDARY,abs(x)) for x,y,z in vertices],a=base.ORANGE,b=base.CREAM)
 shapes=[face]
 def fur(name,points):
  tris=base.front_winding(points,[(0,1,2)])
  shapes.append(dict(name=name,vertices=points,triangles=tris,normals=base.normals(points,tris),
   mask=[y-base.interpolate(base.BOUNDARY,abs(x)) for x,y,z in points],a=base.ORANGE,b=base.CREAM))
 for side in (-1,1):
  for number,(a,tip,b) in enumerate([((18,0),(31,3,-2),(20,6)),((20,4),(33,6,-2),(18,10)),((18,8),(30,10,-1),(13.5,14))]):
   points=[[side*a[0],a[1],surface(*a)+.08],[side*tip[0],tip[1],tip[2]],[side*b[0],b[1],surface(*b)+.08]]
   fur(f'cheek-fur-{side}-{number}',points)
 for number,(a,tip,b) in enumerate([((-6,-22),(-3,-28,-2),(0,-23)),((-2,-23),(1,-30,-2),(3,-22)),((1,-22),(7,-27,-2),(6,-20))]):
  fur(f'forelock-{number}',[[a[0],a[1],surface(*a)+.08],list(tip),[b[0],b[1],surface(*b)+.08]])
 # Reuse ear/eye marker topology, not the previous loft or its depth profile.
 for old in base.author()[1:]:
  if old['name'].startswith(('eye-','iris-','pupil-')):
   lift=.35 if old['name'].startswith('eye-') else .6 if old['name'].startswith('iris-') else .85
   old['vertices']=[[x,y,surface(x,y)+lift] for x,y,z in old['vertices']]
  elif old['name'] in ('nose','mouth'):
   lift=1.3 if old['name']=='nose' else .35
   old['vertices']=[[x,y,surface(x,y)+lift] for x,y,z in old['vertices']]
  shapes.append(old)
 # These checks are causal and independent of a chosen rendering or colour.
 assert all(muzzle_offset(x,y)==0 for x in (-33,-20,-11,11,20,33) for y in (-25,0,8,17))
 assert muzzle_offset(0,8)==14 and muzzle_offset(0,0)==muzzle_offset(0,16)==0
 assert len(vertices)==83 and len(triangles)==144
 return shapes

if __name__=='__main__':
 args=sys.argv[1:] or ['--out-dir',str(base.ROOT/'.cache/fox-surface-separated')]
 config=dict(mode='independent-skull-compact-muzzle-contour-fur',skull=SKULL,muzzle=MUZZLE,
  sourceSha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
  notes=['The skull crown is not inferred from the reference hair-inclusive upper contour.',
   'Muzzle deformation has compact support; outer cheeks do not inherit its forward offset.',
   'Fur silhouette pieces are not smooth skull or cheek volume.'])
 base.main(args,authoring=new_author,authoring_config=config,surface_fn=surface)
