"""Generic analytic tests. No private model or Blender-derived payload."""
import math,numpy as np
from rigid_dq_consumer import rigid_dq,deform,relative_morph,mask_opacity

def rz(t):
 c,s=math.cos(t),math.sin(t);m=np.eye(4);m[:2,:2]=((c,-s),(s,c));return m
# Both signs of quaternion, all largest-diagonal branches, continuous angles.
for angle in np.linspace(-math.pi,math.pi,513):
 m=rz(angle);m[:3,3]=(2,-3,1);q=rigid_dq(m)
 np.testing.assert_allclose(deform((.3,.4,.5),[q],[(0,1)]),(m@(.3,.4,.5,1))[:3],atol=2e-14)
 neg=tuple(-x for x in q)
 np.testing.assert_allclose(deform((.3,.4,.5),[q,neg],[(0,.25),(1,.75)]),(m@(.3,.4,.5,1))[:3],atol=2e-14)
# Symmetric twist preserves cylinder radius; linear blend collapses at 180 deg.
a=rz(math.pi/2);b=rz(-math.pi/2)
np.testing.assert_allclose(deform((1,0,0),[rigid_dq(a),rigid_dq(b)],[(0,.5),(1,.5)]),(1,0,0),atol=1e-14)
for diag in ((1,-1,-1),(-1,1,-1),(-1,-1,1)):
 m=np.eye(4);m[:3,:3]=np.diag(diag);np.testing.assert_allclose(deform((1,2,3),[rigid_dq(m)],[(0,1)]),np.array(diag)*(1,2,3),atol=1e-14)
# Reject scale/shear/reflection, never approximate preserve volume implicitly.
for m in (np.diag((2,1,1,1)),np.diag((-1,1,1,1))):
 try:rigid_dq(m)
 except ValueError:pass
 else:raise AssertionError('nonrigid accepted')
basis=np.array([[0.,0,0],[1,0,0]]);target=basis+(0,.2,0);other=target+(0,0,.4)
for value in np.linspace(0,1,101):
 morphed=relative_morph(basis,[(target,basis,value),(other,target,1-value)])
 np.testing.assert_allclose(morphed,basis+(0,.2*value,.4*(1-value)),atol=1e-15)
 for p in morphed:assert np.isfinite(deform(p,[rigid_dq(rz(value))],[(0,1)])).all()
 for control in (value,1-value):
  expected=1-min(1,50*max(value,control))
  assert mask_opacity((2,0,0),0,-1,1,value,control,50)==expected
  assert mask_opacity((0,0,0),0,-1,1,value,control,50)==1
assert abs(mask_opacity((2,0,0),0,-1,1,.001,0,50)-.95)<1e-14
assert mask_opacity((1,0,0),0,-1,1,1,0,50)==0 # strict boundary
print('PASS 513 rotation/hemisphere cases, relative morph and continuous opacity sweep; reference consumer only')
