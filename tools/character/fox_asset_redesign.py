"""Offline unlit fox authoring study. No product/provider or device changes."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import zlib

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
from make_font import rows as glyph

ORANGE=0xe47b30
CREAM=0xffe8bb
DARK=0x35221d
BG=0xe4ece0

def rgb565(v):return ((v>>19)<<11)|(((v>>10)&63)<<5)|((v>>3)&31)
def unpack(v):return ((v>>11)*255//31,((v>>5)&63)*255//63,(v&31)*255//31)
def interpolate(points,x):
 for (a,u),(b,v) in zip(points,points[1:]):
  if x<=b:return u+(v-u)*(x-a)/(b-a)
 return points[-1][1]

# Author from reference proportions, not from the previous model's vertices.
ROWS=[(-25,0,-4,-4),(-22,9,0,-5),(-17,16,5,-4),(-10,22,9,-4),
      (-3,26,13,-3),(2,27,17,-3),(6.6,33,24,-2),(10,26,24,0),
      (14,17,18,5),(17,0,12,12)]
BOUNDARY=[(0,8),(5,6.5),(11,3.5),(18,3),(27,4),(33,7)]

def surface(x,y):
 width=interpolate([(r[0],r[1]) for r in ROWS],y)
 center=interpolate([(r[0],r[2]) for r in ROWS],y)
 edge=interpolate([(r[0],r[3]) for r in ROWS],y)
 t=min(1,abs(x)/max(width,1e-9))
 return edge+(center-edge)*max(0,1-t*t)**.7

def normals(vertices,triangles):
 result=[[0.,0.,0.] for _ in vertices]
 for a,b,c in triangles:
  u=[vertices[b][i]-vertices[a][i] for i in range(3)]
  v=[vertices[c][i]-vertices[a][i] for i in range(3)]
  n=[u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]]
  assert math.hypot(*n)>1e-8
  if n[2]<0:n=[-x for x in n]
  for k in (a,b,c):
   for i in range(3):result[k][i]+=n[i]
 return [[x/math.hypot(*n) for x in n] for n in result]

def front_winding(vertices,triangles):
 result=[]
 for a,b,c in triangles:
  p,q,r=vertices[a],vertices[b],vertices[c]
  area=(q[0]-p[0])*(r[1]-p[1])-(q[1]-p[1])*(r[0]-p[0])
  assert abs(area)>1e-8,'front-projected triangle must not degenerate'
  result.append((a,b,c) if area>0 else (a,c,b))
 return result

def author():
 vertices=[];rings=[];triangles=[]
 for y,width,_,_ in ROWS:
  ring=[]
  for t in ([0] if width==0 else [-1,-.75,-.5,-.25,0,.25,.5,.75,1]):
   x=width*t;ring.append(len(vertices));vertices.append([x,y,surface(x,y)])
  rings.append(ring)
 for a,b in zip(rings,rings[1:]):
  if len(a)==1:triangles.extend((a[0],b[i],b[i+1]) for i in range(8))
  elif len(b)==1:triangles.extend((a[i],b[0],a[i+1]) for i in range(8))
  else:
   for i in range(8):triangles.extend(((a[i],b[i],b[i+1]),(a[i],b[i+1],a[i+1])))
 triangles=front_winding(vertices,triangles);ns=normals(vertices,triangles)
 masks=[y-interpolate(BOUNDARY,abs(x)) for x,y,z in vertices]
 shapes=[dict(name='continuous-face',vertices=vertices,triangles=triangles,normals=ns,mask=masks,a=ORANGE,b=CREAM)]
 def polygon(name,points,color,triangles=None):
  if triangles is None:
   crosses=[]
   for a,b,c in zip(points,points[1:]+points[:1],points[2:]+points[:2]):
    crosses.append((b[0]-a[0])*(c[1]-b[1])-(b[1]-a[1])*(c[0]-b[0]))
   assert all(v>0 for v in crosses) or all(v<0 for v in crosses),'fan requires convex authoring polygon'
   triangles=[(0,i,i+1) for i in range(1,len(points)-1)]
  shapes.append(dict(name=name,vertices=points,triangles=front_winding(points,triangles),
                     normals=[[0,0,1] for _ in points],mask=[-1]*len(points),a=color,b=color))
 for side in (-1,1):
  # Ear root is behind the face. Silhouette and inner ear are separate cold parts.
  polygon(f'ear-{side}',[[side*x,y,z] for x,y,z in [(9,-16,-6),(11,-34,-8),(23,-55,-10),(30,-40,-8),(26,-15,-4)]],ORANGE)
  polygon(f'inner-ear-{side}',[[side*x,y,z] for x,y,z in [(14,-21,-5.5),(16,-35,-7),(23,-48,-9),(26,-37,-7),(23,-20,-3)]],0xe89978)
  cx=side*10.75;cy=-2.2
  # Flat aperture silhouettes are design markers, not yet an eye/eyelid rig.
  for name,rx,ry,color,lift in [('eye',5.6,6.0,0xfff5dc,.35),('iris',3.2,4.6,0x74401f,.6),('pupil',2.2,3.8,DARK,.85)]:
   points=[]
   for k in range(12):
    t=2*math.pi*k/12;x=cx+rx*math.cos(t);y=cy+ry*math.sin(t)
    points.append([x,y,surface(x,y)+lift])
   polygon(f'{name}-{side}',points,color)
 polygon('nose',[[-3.3,6.3,27.5],[0,5.7,28.5],[3.3,6.3,27.5],[2.2,8.5,27.2],[0,9.3,27],[-2.2,8.5,27.2]],DARK)
 polygon('mouth',[[-5,12,24.0],[0,13,24.5],[5,12,24.0],[0,13.5,24.4]],DARK,[(0,1,3),(1,2,3)])
 return shapes

def native_model(shapes,mode='unlit'):
 nodes=[dict(index=0,name='fox-redesign-root',parent=-1,kind='group',enabled=True,pose=[0,0,0,0,0,0,1,1,1])]
 for shape in shapes:
  batches=[];ids=[];tris=[]
  for tri in shape['triangles']:
   if len(set(ids)|set(tri))>32 or len(tris)>=64:
    batches.append((ids,tris));ids=[];tris=[]
   for index in tri:
    if index not in ids:ids.append(index)
   tris.append(tuple(ids.index(i) for i in tri))
  if tris:batches.append((ids,tris))
  for batch,(ids,tris) in enumerate(batches):
   nodes.append(dict(index=len(nodes),name=f"{shape['name']}-{batch}",parent=0,kind='gouraudMesh',enabled=True,
    pose=[0,0,0,0,0,0,1,1,1],color=shape['a'],thickness=1,opacity=1,edgeSoftness=0,
    material=[1,0,0,32,0,1,1,1,0,0,0] if mode=='unlit' else [.28,.72,0,32,0,1,1,1,0,0,0],mesh=dict(vertices=[v for i in ids for v in shape['vertices'][i]],
    normals=[v for i in ids for v in shape['normals'][i]],mask=[shape['mask'][i] for i in ids],
    indices=[v for t in tris for v in t],order='centroid-depth',holes=[],albedoA=rgb565(CREAM if mode=='clay' else shape['a']),albedoB=rgb565(CREAM if mode=='clay' else shape['b']),thickness=175)))
 return dict(schema='ayatsuri-native-model-design-v1',nodes=nodes)

def raster(shapes,yaw,silhouette=False):
 w,h=240,135;pixels=bytearray(bytes(unpack(rgb565(BG)))*(w*h));depth=[-math.inf]*(w*h)
 angle=math.radians(yaw);c,s=math.cos(angle),math.sin(angle)
 for shape in shapes:
  if silhouette and shape['name']!='continuous-face' and not (
   silhouette=='contour' and shape['name'].startswith(('cheek-fur-','forelock-'))):continue
  points=[(120+1.3*(c*x+s*z),87+1.3*y,-s*x+c*z) for x,y,z in shape['vertices']]
  for indices in shape['triangles']:
   a,b,d=[points[i] for i in indices];det=(b[0]-a[0])*(d[1]-a[1])-(b[1]-a[1])*(d[0]-a[0])
   if abs(det)<1e-8:continue
   for y in range(max(0,math.ceil(min(a[1],b[1],d[1]))),min(h-1,math.floor(max(a[1],b[1],d[1])))+1):
    for x in range(max(0,math.ceil(min(a[0],b[0],d[0]))),min(w-1,math.floor(max(a[0],b[0],d[0])))+1):
     u=((x-a[0])*(d[1]-a[1])-(y-a[1])*(d[0]-a[0]))/det
     v=((b[0]-a[0])*(y-a[1])-(b[1]-a[1])*(x-a[0]))/det
     if u<0 or v<0 or u+v>1:continue
     z=a[2]+u*(b[2]-a[2])+v*(d[2]-a[2]);p=y*w+x
     if z<depth[p]:continue
     depth[p]=z;mask=sum(t*shape['mask'][i] for t,i in zip((1-u-v,u,v),indices))
     color=DARK if silhouette else shape['b'] if mask>=0 else shape['a']
     pixels[p*3:p*3+3]=bytes(unpack(rgb565(color)))
 return pixels

def png(path,w,h,pixels):
 assert len(pixels)==w*h*3,'RGB output must exactly match the declared dimensions'
 def chunk(t,b):return struct.pack('>I',len(b))+t+b+struct.pack('>I',zlib.crc32(t+b)&0xffffffff)
 raw=b''.join(b'\0'+pixels[y*w*3:(y+1)*w*3] for y in range(h))
 path.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(raw,9))+chunk(b'IEND',b''))

def landmark_comparison(shapes,out,surface_fn=None):
 reference=json.loads((ROOT/'tools/character/fox_reference_landmarks.json').read_text())
 w,h=960,310;pixels=bytearray(bytes((24,28,32))*(w*h))
 def line(x0,y0,x1,y1,color):
  steps=max(1,math.ceil(max(abs(x1-x0),abs(y1-y0))))
  for i in range(steps+1):
   x=round(x0+(x1-x0)*i/steps);y=round(y0+(y1-y0)*i/steps)
   if 0<=x<w and 0<=y<h:pixels[(y*w+x)*3:(y*w+x)*3+3]=bytes(color)
 def text(x,y,value):
  for char in value:
   for yy,bits in enumerate(glyph(char)):
    for xx in range(5):
     if bits&(1<<(4-xx)):
      p=((y+yy)*w+x+xx)*3;pixels[p:p+3]=b'\xe9\xee\xe8'
   x+=6
 text(5,5,'OFFLINE UNLIT / RED: MANUAL REFERENCE OUTLINE + LANDMARKS / CYAN: NEW 3D LANDMARKS')
 for col,(key,yaw,cx,top) in enumerate([('FRONT',0,290,82),('THREE_QUARTER',35,650,81)]):
  image=raster(shapes,yaw);x0=col*480;y0=35
  text(x0+5,20,key+' / REFERENCE YAW IS NOT CALIBRATED')
  for y in range(135):
   for x in range(240):
    rgb=image[(y*240+x)*3:(y*240+x)*3+3]
    for dy in (0,1):
     for dx in (0,1):
      p=((y0+2*y+dy)*w+x0+2*x+dx)*3;pixels[p:p+3]=rgb
  view=reference['views'][key]
  def ref(p):return x0+2*(120+(p[0]-cx)*66/172*1.3),y0+2*(87+(-25+(p[1]-top)*42/109)*1.3)
  contour=view['face_outline_without_ears_clockwise']
  for a,b in zip(contour,contour[1:]+contour[:1]):line(*ref(a),*ref(b),(230,60,65))
  for name,p in view.items():
   if ('eye_center' in name or name in ('nose_tip','chin')) and isinstance(p,list) and len(p)==2:
    x,y=ref(p);line(x-3,y,x+3,y,(230,60,65));line(x,y-3,x,y+3,(230,60,65))
  angle=math.radians(yaw)
  depth=surface_fn or surface
  for x,y,z in [(-10.75,-2.2,depth(-10.75,-2.2)+.35),(10.75,-2.2,depth(10.75,-2.2)+.35),
                 (0,6.6,depth(0,6.6)+1.3 if surface_fn else 28.5),(0,17,depth(0,17) if surface_fn else 12)]:
   sx=x0+2*(120+1.3*(math.cos(angle)*x+math.sin(angle)*z));sy=y0+2*(87+1.3*y)
   line(sx-3,sy,sx+3,sy,(0,135,170));line(sx,sy-3,sx,sy+3,(0,135,170))
 png(out/'landmark-comparison.png',w,h,pixels)

def main(argv=None,*,authoring=None,authoring_config=None,surface_fn=None):
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--out-dir',type=Path,default=ROOT/'.cache/fox-asset-redesign')
 out=parser.parse_args(argv).out_dir.resolve();assert out.is_relative_to(ROOT/'.cache');out.mkdir(parents=True,exist_ok=True)
 shapes=(authoring or author)();model=native_model(shapes)
 (out/'model.json').write_text(json.dumps(model,indent=2)+'\n')
 record=dict(rows=ROWS,boundary=BOUNDARY,shapes=shapes) if authoring_config is None else dict(config=authoring_config,shapes=shapes)
 (out/'authoring.json').write_text(json.dumps(record,indent=2)+'\n')
 # Preserve the static native boundary before any mutable-pose integration.
 subprocess.run(['node','-e',"const fs=require('fs'),{compile}=require('./tools/ayatsuri/model/pack_model.cjs');fs.writeFileSync(process.argv[2],compile(JSON.parse(fs.readFileSync(process.argv[1],'utf8'))));",str(out/'model.json'),str(out/'model.aynm')],cwd=ROOT,check=True)
 for mode in ('lit','clay'):
  folder=out/mode;folder.mkdir(exist_ok=True);derived=native_model(shapes,mode)
  (folder/'model.json').write_text(json.dumps(derived,indent=2)+'\n')
  subprocess.run(['node','-e',"const fs=require('fs'),{compile}=require('./tools/ayatsuri/model/pack_model.cjs');fs.writeFileSync(process.argv[2],compile(JSON.parse(fs.readFileSync(process.argv[1],'utf8'))));",str(folder/'model.json'),str(folder/'model.aynm')],cwd=ROOT,check=True)
  data=(folder/'model.aynm').read_bytes()
  for stem in ('fox-patch-compiled','fox-patch-eye','fox-patch-mask','fox-patch-shape','fox-patch-combined'):
   (folder/(stem+'.aynm')).write_bytes(data)
  counts=dict(asset='fox-patch-combined',vertices=sum(len(n['mesh']['vertices'])//3 for n in derived['nodes'][1:]),
              triangles=sum(len(n['mesh']['indices'])//3 for n in derived['nodes'][1:]),meshes=len(derived['nodes'])-1)
  (folder/'fox-patch-combined_data.js').write_text('globalThis.__foxCompiledData='+json.dumps(counts)+';\n')
 modes=(False,True) if authoring_config is None else (False,True,'contour')
 w,h=1440,600 if len(modes)==2 else 885;pixels=bytearray(bytes((24,28,32))*(w*h))
 def text(x,y,value):
  for char in value:
   for yy,bits in enumerate(glyph(char)):
    for xx in range(5):
     if bits&(1<<(4-xx)):
      p=((y+yy)*w+x+xx)*3;pixels[p:p+3]=b'\xe9\xee\xe8'
   x+=6
 text(5,5,'OFFLINE AUTHORING PROJECTION / UNLIT / NOT NATIVE RENDERER OR DEVICE CAPTURE')
 for row,silhouette in enumerate(modes):
  for col,yaw in enumerate((-35,0,35)):
   image=raster(shapes,yaw,silhouette);x0=col*480;y0=30+row*285
   label='FLAT ALBEDO + LANDMARK MARKERS' if not silhouette else 'FACE SILHOUETTE'
   if authoring_config is not None and silhouette:label='COMBINED FACE + FUR' if silhouette=='contour' else 'SKULL ONLY / FUR OMITTED'
   text(x0+5,y0-10,label+f' YAW {yaw:+d}')
   for y in range(135):
    for x in range(240):
     rgb=image[(y*240+x)*3:(y*240+x)*3+3]
     for dy in (0,1):
      for dx in (0,1):
       p=((y0+y*2+dy)*w+x0+x*2+dx)*3;pixels[p:p+3]=rgb
 png(out/'unlit-contact.png',w,h,pixels)
 landmark_comparison(shapes,out,surface_fn)
 proof=dict(kind='OFFLINE AUTHORING ONLY / NOT NATIVE RENDERER OR DEVICE',nativeAssetBytes=(out/'model.aynm').stat().st_size,
  nativeAssetSha256=hashlib.sha256((out/'model.aynm').read_bytes()).hexdigest(),nodes=len(model['nodes']),
  faceVertices=len(shapes[0]['vertices']),faceTriangles=len(shapes[0]['triangles']),
  intendedFrontRatios=dict(faceWidthToHeight=66/42,eyeSeparationToFaceWidth=21.5/66,noseFractionFromForehead=(6.6+25)/42) if authoring_config is None else None,
  authoringConfig=authoring_config,
  referenceLandmarksSha256=hashlib.sha256((ROOT/'tools/character/fox_reference_landmarks.json').read_bytes()).hexdigest(),
  generatorSha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
  limitations=['Reference points are manually estimated; the sheet is not calibrated multiview geometry.',
   'Offline depth buffer is authoring visualization only. Native painter ordering, eye clipping, morph and memory budget are not yet accepted.',
   'Native cold asset compiles, but it is not registered in the product provider. No JS vertex graph is introduced into the product.'])
 (out/'proof.json').write_text(json.dumps(proof,indent=2)+'\n');print(json.dumps(proof))

if __name__=='__main__':main()
