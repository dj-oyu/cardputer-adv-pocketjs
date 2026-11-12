"""Known rigid-transform oracle independent of Blender and product skin math."""
import math,json,sys,hashlib
from pathlib import Path
A=(2**-.5,0,2**-.5);P=(1,0,0);ANGLE=math.pi/3

def add(a,b):return [x+y for x,y in zip(a,b)]
def scale(a,s):return [x*s for x in a]
def dot(a,b):return sum(x*y for x,y in zip(a,b))
def cross(a,b):return [a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]]
def unit(a):
 n=math.sqrt(dot(a,a));assert math.isfinite(n) and n>1e-12;return scale(a,1/n)
def rotate(v,angle):return add(add(scale(v,math.cos(angle)),scale(cross(A,v),math.sin(angle))),scale(A,dot(A,v)*(1-math.cos(angle))))
def qmul(a,b):return [a[0]*b[0]-dot(a[1:],b[1:]),*add(add(scale(b[1:],a[0]),scale(a[1:],b[0])),cross(a[1:],b[1:]))]
def conj(q):return [q[0],*scale(q[1:],-1)]
def dq(q,t):return q,scale(qmul([0,*t],q),.5)
def blend(items,weights):
 assert len(items)==len(weights) and len(items)>0
 assert all(math.isfinite(w) and w>=0 for w in weights) and sum(weights)>0
 for q,d in items:
  assert len(q)==len(d)==4 and all(math.isfinite(x) for x in q+d)
  assert abs(dot(q,q)-1)<1e-8 and abs(dot(q,d))<1e-8
 ref=items[next(i for i,w in enumerate(weights) if w>0)][0];r=[0.]*4;d=[0.]*4
 for (q,dual),w in zip(items,weights):
  sign=-1 if dot(ref,q)<0 else 1;r=add(r,scale(q,w*sign));d=add(d,scale(dual,w*sign))
 n=math.sqrt(dot(r,r));assert n>1e-12;r=scale(r,1/n);d=scale(d,1/n);d=add(d,scale(r,-dot(r,d)))
 return r,d

def apply(pair,v):
 q,d=pair;t=scale(qmul(d,conj(q))[1:],2);return add(qmul(qmul(q,[0,*v]),conj(q))[1:],t)
def near(a,b,t=2e-6):assert len(a)==len(b) and all(math.isfinite(x) and abs(x-y)<=t for x,y in zip(a,b)),(a,b)
def verify(r):
 assert r['schema']=='public-dq-fixture-v1'
 vertices=[[x,y,0] for y in (-1,1) for x in (-2,0,2)];faces=[[0,1,4],[0,4,3],[1,2,5],[1,5,4]]
 assert r['vertices']==vertices and r['faces']==faces and r['weights']==[[1-w,w] for w in [0,.5,1]*2]
 q=[math.cos(ANGLE/2),*scale(A,math.sin(ANGLE/2))];t=add(P,scale(rotate(P,ANGLE),-1));items=[dq([1,0,0,0],[0,0,0]),dq(q,t)]
 predictions={'DQ':[],'LBS':[]};transported=[];maxerror=0
 for i,v in enumerate(vertices):
  w=[0,.5,1][i%3];phi=2*math.atan2(w*math.sin(ANGLE/2),1-w+w*math.cos(ANGLE/2))
  expected=add(P,rotate(add(v,scale(P,-1)),phi));near(apply(blend(items,[1-w,w]),v),expected,1e-12)
  predictions['DQ'].append(expected);transported.append(rotate([0,0,1],phi))
  transformed=add(P,rotate(add(v,scale(P,-1)),ANGLE));predictions['LBS'].append(add(scale(v,1-w),scale(transformed,w)))
 for mode,vs in predictions.items():
  assert len(r['samples'][mode]['positions'])==6 and len(r['samples'][mode]['faceNormals'])==4
  for v,actual in zip(vs,r['samples'][mode]['positions']):near(actual,v);maxerror=max(maxerror,max(abs(x-y) for x,y in zip(v,actual)))
  for f,actual in zip(faces,r['samples'][mode]['faceNormals']):
   a,b,c=[vs[i] for i in f];near(actual,unit(cross(add(b,scale(a,-1)),add(c,scale(a,-1)))))
 gap=max(math.dist(a,b) for a,b in zip(predictions['DQ'],predictions['LBS']));assert gap>.01
 for w in (0,.5,1):
  opposite=(scale(items[1][0],-1),scale(items[1][1],-1));near(apply(blend([items[0],opposite],[1-w,w]),[0,1,0]),apply(blend(items,[1-w,w]),[0,1,0]),1e-12)
 rejected=0
 def rejects(fn):
  nonlocal rejected
  try:fn()
  except AssertionError:rejected+=1
  else:raise AssertionError('negative accepted')
 for weights in ([0,0],[-1,2],[float('nan'),1]):rejects(lambda:blend(items,weights))
 for malformed in [([0,0,0,0],[0,0,0,0]),([2,0,0,0],[0,0,0,0]),([1,0,0,0],[1,0,0,0]),([float('nan'),0,0,0],[0,0,0,0])]:rejects(lambda:blend([items[0],malformed],[1,0]))
 rejects(lambda:near(predictions['LBS'][1],predictions['DQ'][1]))
 wrong=(scale(items[1][0],-1),items[1][1]);rejects(lambda:near(apply(blend([items[0],wrong],[.5,.5]),[0,1,0]),apply(blend(items,[.5,.5]),[0,1,0])))
 unnormalized=(scale(add(items[0][0],items[1][0]),.5),scale(add(items[0][1],items[1][1]),.5));rejects(lambda:near(apply(unnormalized,[0,1,0]),apply(blend(items,[.5,.5]),[0,1,0])))
 return {'status':'PASS','maxBlenderPositionError':maxerror,'maxDqLbsDistance':gap,'negativeCases':rejected,'positions':predictions,'transportedNormalsDQ':transported,'normalSemantics':'face normals compared with deformed geometry; transported normals are fixed-weight predictions, not surface normals','scope':'public synthetic rigid transforms only; no product DQ implementation or wire change'}
if __name__=='__main__':
 p=Path(sys.argv[1]);raw=p.read_bytes();result=verify(json.loads(raw));assert raw==p.read_bytes();result['receiptSha256']=hashlib.sha256(raw).hexdigest();result['sourceSha256']={Path(p).name:hashlib.sha256(Path(p).read_bytes()).hexdigest() for p in [__file__,Path(__file__).with_name('blender_dq_fixture.py')]};print(json.dumps(result,indent=2))
