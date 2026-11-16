"""Slow host-only orthographic triangle/alpha reference, not production renderer.
Pixel samples are integer x/y like the current mesh raster; lighting is flat
linear RGB to isolate visibility/depth/alpha. No PBR or perspective claim.
"""
import math

def render(triangles,width,height,slots=4,span_limit=54):
 if width<1 or height<1 or slots<1:raise ValueError('limits')
 opaque=[(-math.inf,(0.,0.,0.)) for _ in range(width*height)];fragments=[[] for _ in opaque]
 for order,t in enumerate(triangles):
  if type(t.get('visible',True)) is not bool:raise ValueError('visibility')
  if not t.get('visible',True):continue
  v=[tuple(x) for x in t['vertices']];color=tuple(t['color']);alpha=t['alpha']
  if len(v)!=3 or any(len(x)!=4 for x in v) or len(color)!=3 or not all(math.isfinite(x) for p in v for x in p) or not all(math.isfinite(x) and 0<=x<=1 for x in color) or not math.isfinite(alpha) or not 0<=alpha<=1:raise ValueError('invalid triangle')
  # Fourth coordinate is authored object-local mask coordinate, not screen x.
  mask=t.get('mask')
  if mask and (len(mask)!=2 or not all(math.isfinite(x) for x in mask) or mask[0]>mask[1]):raise ValueError('mask')
  def edge(a,b,x,y):return (b[0]-a[0])*(y-a[1])-(b[1]-a[1])*(x-a[0])
  area=edge(v[0],v[1],v[2][0],v[2][1])
  if area==0:continue
  if area<0:v[1],v[2]=v[2],v[1];area=-area
  for y in range(max(0,math.ceil(min(p[1] for p in v))),min(height-1,math.floor(max(p[1] for p in v)))+1):
   for x in range(max(0,math.ceil(min(p[0] for p in v))),min(width-1,math.floor(max(p[0] for p in v)))+1):
    pairs=((v[1],v[2]),(v[2],v[0]),(v[0],v[1]));es=[edge(a,b,x,y) for a,b in pairs]
    if any(e<0 or (e==0 and not (b[1]>a[1] or (b[1]==a[1] and b[0]<a[0]))) for e,(a,b) in zip(es,pairs)):continue
    w=[e/area for e in es];z=sum(w[i]*v[i][2] for i in range(3));local=sum(w[i]*v[i][3] for i in range(3));a=1. if mask and mask[0]<local<mask[1] else alpha;at=y*width+x
    if a==1:
     if z>opaque[at][0]:opaque[at]=(z,color)
    elif a>0:fragments[at].append((z,order,color,a))
 # Late errors only discard this returned candidate; no committed output input.
 for y in range(height):
  used=[]
  for x in range(width):
   at=y*width+x;fragments[at]=[f for f in fragments[at] if f[0]>opaque[at][0]]
   if len(fragments[at])>slots:raise ValueError('slot overflow')
   if fragments[at]:used.append(x)
  if used and used[-1]-used[0]+1>span_limit:raise ValueError('row span overflow')
 result=[]
 for (z,color),fs in zip(opaque,fragments):
  for depth,order,rgb,a in sorted(fs,key=lambda f:(f[0],-f[1])):color=tuple(rgb[k]*a+color[k]*(1-a) for k in range(3))
  result.append(color)
 return tuple(result)
