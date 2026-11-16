import ctypes as C,pathlib,subprocess,tempfile,numpy as np
from depth_clip_reference import clip_triangle
r=pathlib.Path(__file__).parent.resolve();tmp=pathlib.Path(tempfile.mkdtemp(prefix='alpha-clip-'))
(tmp/'wrap.c').write_text('''#include "alpha_packet_candidate.h"
int clip(const float *v,float lo,float hi,float *out){hone_alpha_packet p={0},q[3];memcpy(p.v,v,12*sizeof(float));p.alpha=.37f;int n=hone_alpha_clip(&p,lo,hi,q);if(n<0)return n;for(int i=0;i<n;i++)memcpy(out+12*i,q[i].v,12*sizeof(float));return n;}
''')
subprocess.run(['cc','-O2','-shared','-fPIC','-I'+str(r),str(tmp/'wrap.c'),'-lm','-o',str(tmp/'clip.so')],check=True)
f=C.CDLL(str(tmp/'clip.so')).clip;F=C.POINTER(C.c_float);f.argtypes=[F,C.c_float,C.c_float,F];rng=np.random.default_rng(84);worst=0
for _ in range(1000):
 v=rng.uniform(-3,3,(3,4)).astype(np.float32);out=np.zeros(36,np.float32);n=f(v.ctypes.data_as(F),-.7,1.1,out.ctypes.data_as(F));ref=np.array(clip_triangle(v.tolist(),float(np.float32(-.7)),float(np.float32(1.1))))
 assert n==len(ref)
 if n:
  error=float(np.max(np.abs(out[:n*12].reshape(n,3,4)-ref)));worst=max(worst,error);assert error<1e-5
print(dict(random_cases=1000,max_component_error=worst,scope='synthetic native clip vs independent Python reference'))
