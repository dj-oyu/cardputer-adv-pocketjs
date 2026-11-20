"""Generic synthetic tests; no model-derived fixtures."""
import ctypes as C
import pathlib, subprocess, tempfile
import numpy as np
r=pathlib.Path(__file__).parent
lib=pathlib.Path(tempfile.gettempdir())/'nearest_batch.so'
subprocess.run(['cc','-O2','-shared','-fPIC',str(r/'nearest_batch_candidate.c'),'-lm','-o',str(lib)],check=True)
l=C.CDLL(str(lib)); D=C.POINTER(C.c_double); U=C.POINTER(C.c_uint32)
l.nearest_surface.argtypes=[D,D,C.c_size_t,U,C.c_size_t,D,D,U]
l.nearest_batch.argtypes=[D,C.c_size_t,D,C.c_size_t,U,C.c_size_t,C.c_void_p,C.c_size_t,D,U,C.POINTER(C.c_size_t)]
def run(v,t,p,capacity=None):
 v=np.ascontiguousarray(v,dtype=float);t=np.ascontiguousarray(t,dtype=np.uint32);p=np.ascontiguousarray(p,dtype=float)
 nb=(len(t)+7)//8;box=np.zeros((nb,6),np.float32);out=np.zeros_like(p);faces=np.zeros(len(p),np.uint32);count=C.c_size_t()
 ok=l.nearest_batch(p.ctypes.data_as(D),len(p),v.ctypes.data_as(D),len(v),t.ctypes.data_as(U),len(t),box.ctypes.data,nb if capacity is None else capacity,out.ctypes.data_as(D),faces.ctypes.data_as(U),C.byref(count))
 assert ok
 for i,point in enumerate(p):
  ref=np.zeros(3);w=np.zeros(3);face=C.c_uint32()
  assert l.nearest_surface(point.ctypes.data_as(D),v.ctypes.data_as(D),len(v),t.ctypes.data_as(U),len(t),ref.ctypes.data_as(D),w.ctypes.data_as(D),C.byref(face))
  np.testing.assert_array_equal(out[i],ref);assert faces[i]==face.value
 return count.value
if __name__=='__main__':
 rng=np.random.default_rng(953);n=0
 for scale in (1e-20,1e-8,1,1e8,1e12):
  v=rng.normal(size=(96,3))*scale;t=np.arange(96,dtype=np.uint32).reshape(-1,3);p=rng.normal(size=(100,3))*scale
  for shift in (0,.001,10):
   v+=shift*scale;n+=len(p);run(v,t,p);run(v,t,p,0)
 # Exact repeated triangles across batch boundary preserve first-face tie.
 v=np.array([[0,0,0],[1,0,0],[0,1,0]],float);t=np.tile([0,1,2],(24,1));run(v,t,np.array([[.2,.2,0],[.2,.2,np.nextafter(0.,1.)]]))
 # All-degenerate geometry and same-pointer deformation are recomputed.
 v[:]=0;run(v,t,np.array([[1,2,3.]]));v[:]=3;run(v,t,np.array([[1,2,3.]]))
 print({'synthetic_queries':n+4,'exact_reference_and_face':True,'insufficient_capacity_fallback':True})
