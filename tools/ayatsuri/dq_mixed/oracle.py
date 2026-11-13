"""Public common-pivot two-joint DQ closed form; no product implementation import."""
import math,struct,json,pathlib
f32=lambda v:struct.unpack('<f',struct.pack('<f',v))[0]
I=[1,0,0,0,0,1,0,0,0,0,1,0];weights=[0,32767,32768,65535]
rows=[];largest_gap=0
for degrees in (30,60,120):
 angle=math.radians(degrees);c=math.cos(angle);s=math.sin(angle)
 G=I+[1,0,0,0,0,f32(c),f32(-s),1,0,f32(s),f32(c),0]
 ib1=I[:];ib1[7]=-1;IB=I+ib1
 for aperture in (1,.5):
  vertices=[(-1,0,0),(1,0,0),(1,2*aperture,0),(-1,2*aperture,0)]
  V=[];J=[];W=[];expected=[];lbs=[]
  for (x,y,z),w in zip(vertices,weights):
   t=w/65535.;phi=2*math.atan2(t*math.sin(angle/2),1-t+t*math.cos(angle/2))
   pos=[x,1+(y-1)*math.cos(phi),(y-1)*math.sin(phi)]
   normal=[0,-math.sin(phi),math.cos(phi)]
   linear=[x,(1-t)*y+t*(1+(y-1)*c),t*(y-1)*s]
   largest_gap=max(largest_gap,math.dist(pos,linear))
   V.extend([x,y,z,0,0,1]);J.extend([0,1,0,0]);W.extend([65535-w,w,0,0]);expected.extend(pos+normal);lbs.extend(linear)
  assert len(G)==len(IB)==len(V)==len(expected)==24
  assert len(J)==len(W)==16 and all(sum(W[i:i+4])==65535 for i in range(0,16,4))
  assert all(0<=j<2 for j in J) and all(math.isfinite(v) for v in G+IB+V+expected)
  rows.append({'degrees':degrees,'aperture':aperture,'meshId':1,'G':G,'IB':IB,'V':V,'J':J,'W':W,'expected':expected,'lbsPositions':lbs,'indices':[0,1,2,0,2,3]})
assert largest_gap>.1
# Single-joint endpoints and mixed-half distinction are required observations.
for r in rows:
 assert r['expected'][:3]==r['V'][:3]
 assert all(abs(math.sqrt(sum(v*v for v in r['expected'][i+3:i+6]))-1)<1e-12 for i in range(0,24,6))
result={'schema':'public-common-pivot-mixed-dq-v1','positionNormalTolerance':2e-6,'normalSemantics':'fixed-weight transported unit normal; not deformed face normal','maxDqLbsGap':largest_gap,'cases':rows}
pathlib.Path('expected.json').write_text(json.dumps(result,indent=2));print('PASS',len(rows),largest_gap)
