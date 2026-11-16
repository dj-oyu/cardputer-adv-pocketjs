"""Independent rigid dual-quaternion reference consumer; not runtime admission.

Accepts resolved same-frame palettes, not Blender drivers/constraints. No scale,
shear, reflection, envelopes or implicit approximation to preserve-volume skin.
"""
import math
import numpy as np

def mul(a,b):
 w,x,y,z=a;v,i,j,k=b
 return np.array((w*v-x*i-y*j-z*k,w*i+x*v+y*k-z*j,w*j-x*k+y*v+z*i,w*k+x*j-y*i+z*v))
def conj(q):return q*np.array((1,-1,-1,-1))
def rigid_dq(m):
 m=np.asarray(m,dtype=float)
 if m.shape!=(4,4) or not np.isfinite(m).all() or not np.allclose(m[3],(0,0,0,1),atol=1e-7,rtol=0):raise ValueError('affine matrix required')
 r=m[:3,:3]
 if not np.allclose(r.T@r,np.eye(3),atol=2e-6,rtol=0) or abs(np.linalg.det(r)-1)>2e-6:raise ValueError('rigid proper rotation only')
 # Quaternion from symmetric rotation representation, largest diagonal branch.
 terms=np.array((1+np.trace(r),1+r[0,0]-r[1,1]-r[2,2],1-r[0,0]+r[1,1]-r[2,2],1-r[0,0]-r[1,1]+r[2,2]))
 i=int(np.argmax(terms));q=np.zeros(4);q[i]=.5*math.sqrt(max(0,terms[i]));d=4*q[i]
 if i==0:q[1:]=((r[2,1]-r[1,2])/d,(r[0,2]-r[2,0])/d,(r[1,0]-r[0,1])/d)
 elif i==1:q[[0,2,3]]=((r[2,1]-r[1,2])/d,(r[0,1]+r[1,0])/d,(r[0,2]+r[2,0])/d)
 elif i==2:q[[0,1,3]]=((r[0,2]-r[2,0])/d,(r[0,1]+r[1,0])/d,(r[1,2]+r[2,1])/d)
 else:q[:3]=((r[1,0]-r[0,1])/d,(r[0,2]+r[2,0])/d,(r[1,2]+r[2,1])/d)
 q/=np.linalg.norm(q)
 return q,.5*mul(np.r_[0,m[:3,3]],q)
def deform(point,palette,weights):
 p=np.asarray(point,dtype=float)
 if p.shape!=(3,) or not np.isfinite(p).all():raise ValueError('finite point required')
 if not weights:return p.copy()
 if any(j<0 or j>=len(palette) or not math.isfinite(w) or w<0 for j,w in weights):raise ValueError('invalid influence')
 positive=[(j,w) for j,w in weights if w>0]
 if not positive:return p.copy()
 ref=palette[positive[0][0]][0];a=np.zeros(4);b=np.zeros(4)
 for j,w in positive:
  q,d=palette[j];s=-1 if np.dot(ref,q)<0 else 1;a+=w*s*q;b+=w*s*d
 length=np.linalg.norm(a)
 if length<1e-12:raise ValueError('ambiguous rotation blend')
 a/=length;b/=length;b-=a*np.dot(a,b)
 return mul(mul(a,np.r_[0,p]),conj(a))[1:]+2*mul(b,conj(a))[1:]
def relative_morph(basis,shapes):
 out=np.array(basis,dtype=float,copy=True)
 if not np.isfinite(out).all():raise ValueError('nonfinite basis')
 for target,relative,value in shapes:
  t=np.asarray(target);r=np.asarray(relative)
  if t.shape!=out.shape or r.shape!=out.shape or not np.isfinite(t).all() or not np.isfinite(r).all() or not math.isfinite(value):raise ValueError('invalid relative morph')
  out+=value*(t-r)
 return out

def mask_opacity(object_position,axis,lower,upper,both,side,gain):
 """Transparent->opaque mix, strict band comparisons, continuous driver.
 Caller supplies render-object-local coordinates, not world coordinates.
 This scalar subset does not claim arbitrary node graph or rendered alpha.
 """
 values=(*object_position,lower,upper,both,side,gain)
 if axis not in (0,1,2) or not all(math.isfinite(x) for x in values) or lower>upper or gain<0 or not (0<=both<=1 and 0<=side<=1):raise ValueError('invalid mask input')
 inside=float(lower<object_position[axis]<upper)
 return max(inside,1-min(1,gain*max(both,side)))
