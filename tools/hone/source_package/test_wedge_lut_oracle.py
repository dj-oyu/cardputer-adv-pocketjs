"""Held-out synthetic angle sweep against Blender; does not use private assets."""
import bpy,math,random,ctypes as c,pathlib,sys
fn=c.CDLL(str(pathlib.Path(sys.argv[sys.argv.index('--')+1]).resolve())).polygon_normals
fn.argtypes=[c.POINTER(c.c_float),c.c_size_t,c.POINTER(c.c_uint32),c.POINTER(c.c_uint32),c.POINTER(c.c_uint8),c.c_size_t,c.c_size_t,c.POINTER(c.c_double),c.POINTER(c.c_float)]
rng=random.Random(83);dots=[-1.,-.999999,-.99,-.5,0.,.5,.99,.999999,1.]+[rng.uniform(-1,1) for _ in range(257)];maximum=0
for d in dots:
 pts=[(0,0,0),(1,0,0),(d,math.sqrt(max(0,1-d*d)),0),(0,1,0),(0,0,1)];faces=[[0,1,2],[0,3,4]]
 m=bpy.data.meshes.new('held-out-angle');m.from_pydata(pts,[],faces)
 for p in m.polygons:p.use_smooth=True
 m.update();ref=[v for n in m.corner_normals for v in n.vector]
 out=(c.c_float*18)();work=(c.c_double*15)()
 accepted=fn((c.c_float*15)(*[v for p in pts for v in p]),5,(c.c_uint32*3)(0,3,6),(c.c_uint32*6)(0,1,2,0,3,4),(c.c_uint8*2)(1,1),2,6,work,out)
 if d==-1.:
  assert not accepted;assert any(sum(v*v for v in ref[i:i+3])==0 for i in range(0,len(ref),3));bpy.data.meshes.remove(m);continue
 assert accepted
 err=max(abs(a-b) for a,b in zip(out,ref));maximum=max(maximum,err);
 if err>1e-4:print('FAIL dot',d,'err',err,'ref',ref,'out',list(out))
 bpy.data.meshes.remove(m)
assert maximum<1e-4,maximum
print('PASS',len(dots),'held-out/boundary synthetic wedges; max component error',maximum)
