"""Synthetic expression grammar and Euler convention regression."""
import math,numpy as np
from driver_subset import evaluate,euler_xyz_matrix
for a in np.linspace(-2,2,101):
 for b in (-1,0,.25,1,2):
  v=dict(a=a,b=b)
  assert evaluate('max(0,min(1,a+b))',v)==max(0,min(1,a+b))
  assert evaluate('1 if a==1 else 0',v)==float(a==1)
  assert evaluate('max(a,b)>=0.75',v)==float(max(a,b)>=.75)
  assert evaluate('max(0,min(1,a))*(1-max(0,min(1,b)))',v)==max(0,min(1,a))*(1-max(0,min(1,b)))
  for exponent in range(5):assert evaluate(f'a**{exponent}',v)==a**exponent
  m=euler_xyz_matrix(a,b,-a)
  np.testing.assert_allclose(m[:3,:3].T@m[:3,:3],np.eye(3),atol=1e-14)
for x in ('obj.attr','__import__("os")','[x for x in y]','a/2','a**5','a**b','a**-1','a**2.0','max(a,b,c)','float("nan")'):
 try:evaluate(x,dict(a=1,b=2,c=3))
 except (ValueError,SyntaxError):pass
 else:raise AssertionError(x)
try:evaluate('a',{'a':float('nan')})
except ValueError:pass
else:raise AssertionError('nan accepted')
for expression in ('a**0','a==1'):
 try:evaluate(expression,{'a':float('nan')})
 except ValueError:pass
 else:raise AssertionError('nan input hidden by '+expression)
print('PASS 505 analytic driver/rotation cases and unsupported grammar rejection; no general Blender compatibility claim')
