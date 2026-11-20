"""Synthetic Blender comparison, requires a separately built polygon.so argument."""
import bpy,ctypes as c,sys,pathlib
lib=c.CDLL(str(pathlib.Path(sys.argv[sys.argv.index('--')+1]).resolve()));fn=lib.polygon_normals
fn.argtypes=[c.POINTER(c.c_float),c.c_size_t,c.POINTER(c.c_uint32),c.POINTER(c.c_uint32),c.POINTER(c.c_uint8),c.c_size_t,c.c_size_t,c.POINTER(c.c_double),c.POINTER(c.c_float)]
maxerr=0
for w in [0.,.0001,.001,.01,.25,1.]:
 coords=[[-.5,-.7,2.5],[-.5,-.7+w*.01,2.5],[-.3,-.6,2.7],[-.3,-.6+w*.01,2.7+w*.002],[-.1,-.5,2.8],[-.1,-.5+w*.01,2.8]]
 polys=[[0,1,3,2],[2,3,5,4]];m=bpy.data.meshes.new('synthetic-near-collapse');m.from_pydata(coords,[],polys)
 for p in m.polygons:p.use_smooth=True
 m.update();refs=[v for n in m.corner_normals for v in n.vector]
 ps=(c.c_float*18)(*[v for p in coords for v in p]);off=(c.c_uint32*3)(0,4,8);vi=(c.c_uint32*8)(*[v for p in polys for v in p]);sm=(c.c_uint8*2)(1,1);work=(c.c_double*18)();out=(c.c_float*24)()
 assert fn(ps,6,off,vi,sm,2,8,work,out)==1
 err=max(abs(a-b) for a,b in zip(out,refs));maxerr=max(maxerr,err)
 bpy.data.meshes.remove(m)
assert maxerr<1e-4,maxerr
print('PASS six synthetic near-collapse samples; max component error',maxerr,'runtime not admitted')
