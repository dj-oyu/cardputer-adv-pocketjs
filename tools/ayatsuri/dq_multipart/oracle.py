"""Local multipart contract based on public weight-normal closed form, not a runtime."""
import math, struct, json, pathlib, copy
F=lambda x:struct.unpack('<f',struct.pack('<f',x))[0]
I=[1,0,0,0,0,1,0,0,0,0,1,0]
def part(node,angle,weights,tx,aperture):
 a=math.radians(angle);c=F(math.cos(a));s=F(math.sin(a))
 g=I+[1,0,0,0,0,c,-s,1,0,s,c,0];ib=I+[1,0,0,0,0,1,0,-1,0,0,1,0]
 v=[];j=[];w=[];local=[];world=[]
 for (x,y),q in zip([(-1,0),(1,0),(1,2),(-1,2)],weights):
  y=F(y*aperture);t=q/65535;phi=2*math.atan2(t*math.sin(a/2),1-t+t*math.cos(a/2))
  p=[x,1+(y-1)*math.cos(phi),(y-1)*math.sin(phi)];n=[0,-math.sin(phi),math.cos(phi)]
  v += [x,y,0,0,0,1];j += [0,1,0,0];w += [65535-q,q,0,0]
  local += p+n;world += [p[0]+tx,p[1],p[2]]+n
 return dict(meshId=node,nodeTranslation=[tx,0,0],angleDegrees=angle,G=g,IB=ib,V=v,J=j,W=w,expectedLocal=local,expectedWorld=world)
def pack(parts):
 out={'D':sum(([p['meshId'],4,2] for p in parts),[])}
 for k in ('G','IB','V','J','W'):out[k]=sum((p[k] for p in parts),[])
 return out
def validate(b):
 d=b['D'];assert len(d)==6 and len(set(d[::3]))==2
 for k,n in [('G',48),('IB',48),('V',48),('J',32),('W',32)]:
  assert len(b[k])==n and all(type(x) in (int,float) and math.isfinite(x) for x in b[k])
 assert d[1::3]==[4,4] and d[2::3]==[2,2]
 assert all(type(j)==int and 0<=j<2 for j in b['J'])
 assert all(type(w)==int and 0<=w<=65535 for w in b['W'])
 assert all(sum(b['W'][i:i+4])==65535 for i in range(0,32,4))
cases=[]
for aperture in (1,.5):
 parts=[part(1,30,[0,16384,49151,65535],-3,aperture),part(2,-60,[65535,32768,32767,0],3,aperture)]
 for order in (parts,parts[::-1]):
  b=pack(order);validate(b)
  # Descriptor order moves every span together, while node-relative results stay fixed.
  for i,p in enumerate(order):
   assert b['G'][24*i:24*(i+1)]==p['G'] and b['V'][24*i:24*(i+1)]==p['V']
   assert all(abs(sum(p['expectedLocal'][v+3+k]**2 for k in range(3))-1)<1e-12 for v in range(0,24,6))
  cases.append({'aperture':aperture,'parts':order,'batch':b})
negative=0
for key,index,value in [('D',3,1),('D',4,33),('J',16,2),('J',18,2),('W',16,1),('G',24,float('nan'))]:
 b=copy.deepcopy(cases[0]['batch']);b[key][index]=value
 try:validate(b)
 except AssertionError:negative+=1
 else:raise AssertionError((key,index,'accepted'))
r={'schema':'public-multipart-rigid-contract-v1','componentTolerance':2e-6,'cases':cases,'negativeContractCases':negative,'limitations':'contract validator only; raw native, snapshot, OOM, surface normals and private model equivalence untested'}
pathlib.Path(__file__).with_name('multipart_expected.json').write_text(json.dumps(r,indent=2)+'\n')
print('PASS multipart cases',len(cases),'negative contract cases',negative)
