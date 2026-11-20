"""C consumer vs independent Python rigid transform/DQ reference."""
import ctypes as c,pathlib,subprocess,tempfile,math
import numpy as np
from rigid_dq_consumer import rigid_dq,deform
r=pathlib.Path(__file__).parent
with tempfile.TemporaryDirectory() as d:
 so=pathlib.Path(d)/'dq.so';subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-fPIC','-shared',str(r/'rigid_dq_candidate.c'),'-lm','-o',str(so)],check=True)
 f=c.CDLL(str(so)).rigid_dq_position;D=c.POINTER(c.c_double);U=c.POINTER(c.c_uint);f.argtypes=[D,D,c.c_size_t,U,D,c.c_size_t,D];f.restype=c.c_int
 rng=np.random.default_rng(624)
 maximum=0
 for i in range(1000):
  joints=[];pal=[]
  for j in range(4):
   q=rng.normal(size=4);q/=np.linalg.norm(q);t=rng.uniform(-2,2,size=3);joints.extend((*q,*t))
   from rigid_dq_consumer import mul
   pal.append((q,.5*mul(np.r_[0,t],q)))
  w=rng.uniform(0,1,4);p=rng.normal(size=3);out=(c.c_double*3)(99,99,99)
  assert f((c.c_double*3)(*p),(c.c_double*28)(*joints),4,(c.c_uint*4)(0,1,2,3),(c.c_double*4)(*w),4,out)==1
  maximum=max(maximum,float(np.max(np.abs(np.array(out)-deform(p,pal,list(enumerate(w)))))))
 # Malformed late influence must not expose partial output.
 p=(c.c_double*3)(1,2,3);j=(c.c_double*7)(1,0,0,0,0,0,0);out=(c.c_double*3)(7,8,9)
 assert not f(p,j,1,(c.c_uint*2)(0,1),(c.c_double*2)(.5,.5),2,out)
 assert list(out)==[7,8,9]
 assert f(p,j,1,(c.c_uint*1)(0),(c.c_double*1)(1),1,out) and list(out)==[1,2,3]
 assert maximum<1e-12,maximum
 print('PASS native 1000 random blends; max reference delta',maximum,'late failure unchanged and retry success')
