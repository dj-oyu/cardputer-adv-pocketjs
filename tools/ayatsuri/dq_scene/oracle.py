"""Generic aperture -> rigid skin input and independently stated expected positions."""
import math,struct,json,pathlib
f32=lambda v:struct.unpack('<f',struct.pack('<f',v))[0]
rows=[];worst=0;order_gaps=[]
for side in (-1,1):
 for blink in (0,.5,.97999999):
  a=f32(1-blink)
  for sign in (-1,1):
   c=f32(math.sqrt(3)/2);s=f32(sign*.5)
   G=[1,0,0,2,0,c,-s,-1,0,s,c,3];IB=[1,0,0,0,0,1,0,0,0,0,1,0]
   cold=[(3*side-1,-1,0),(3*side+1,-1,0),(3*side+1,1,0),(3*side-1,1,0)]
   V=[];expected=[]
   for x,y,z in cold:
    py=f32(a*y);V.extend([x,py,0,0,0,1])
    # Independent closed form, not matrix multiplication: aperture then Rx30 then t.
    truth=[x+2,(math.sqrt(3)/2)*py-1,sign*.5*py+3,0,-sign*.5,math.sqrt(3)/2]
    matrix=[sum(G[i*4+j]*[x,py,0][j] for j in range(3))+G[i*4+3] for i in range(3)]+[G[2],G[6],G[10]]
    worst=max(worst,max(abs(x-y) for x,y in zip(matrix,truth)))
    assert max(abs(x-y) for x,y in zip(matrix,truth))<1e-6
    expected.extend(truth)
    # Deliberately wrong ordering: Rx first, then aperture in world Y.
    wrong=[x+2,a*((math.sqrt(3)/2)*y-1),sign*.5*y+3]
    if blink>0:order_gaps.append(max(abs(x-y) for x,y in zip(wrong,truth[:3])))
   assert all(math.isfinite(v) for v in V+G+IB+expected)
   rows.append({'side':side,'blinkNumber':blink,'visible':blink<.98,'angleDegrees':sign*30,'G':G,'IB':IB,'V':V,'J':[0]*16,'W':[65535,0,0,0]*4,'expected':expected,'indices':[0,1,2,0,2,3]})
assert min(order_gaps)>.1
# Ordering and malformed-input guard examples; this is not a product validator.
def validate(row):
 if len(row['G'])!=12 or len(row['IB'])!=12 or len(row['V'])!=24:raise ValueError('shape')
 if not all(math.isfinite(x) for name in ('G','IB','V') for x in row[name]):raise ValueError('finite')
 if row['J']!=[0]*16 or row['W']!=[65535,0,0,0]*4:raise ValueError('one joint only')
for row in rows:validate(row)
import copy
badcases=[]
for key,value in [('G',[0]*11),('IB',[float('nan')]+[0]*11),('V',[0]*23),('J',[1]*16),('W',[0]*16)]:
 b=copy.deepcopy(rows[0]);b[key]=value;badcases.append(b)
for b in badcases:
 try:validate(b)
 except ValueError:pass
 else:raise AssertionError('bad accepted')
result={'schema':'public-aperture-before-rigid-v1','scope':'predeformed local positions then rigid single joint; not product morph or private material','maxMatrixVsClosedForm':worst,'minWrongOrderGap':min(order_gaps),'negativeCases':len(badcases),'cases':rows}
pathlib.Path('expected.json').write_text(json.dumps(result,indent=2));print('PASS',len(rows),worst,min(order_gaps))
