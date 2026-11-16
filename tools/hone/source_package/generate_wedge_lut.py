"""Measure Blender angle-weight responses using newly constructed synthetic wedges.
No Blender implementation code or approximation coefficients are copied.
"""
import bpy,math,json,pathlib,sys
N=64;values=[]
for i in range(N+1):
 d=min(i/N,1.-1e-6);s=math.sqrt(1-d*d)
 m=bpy.data.meshes.new('synthetic-angle-wedge')
 # Two smooth triangles share only the origin. Normals are +Z and +X.
 m.from_pydata([(0,0,0),(1,0,0),(d,s,0),(0,1,0),(0,0,1)],[],[(0,1,2),(0,3,4)])
 for f in m.polygons:f.use_smooth=True
 m.update();v=m.vertices[0].normal
 assert abs(v.y)<1e-6 and v.x>0
 ratio=v.z/v.x
 values.append(ratio/math.sqrt(1-d))
 bpy.data.meshes.remove(m)
# Avoid a nearly degenerate training wedge at the endpoint; extrapolate the smooth scaled response.
values[-1]=2*values[-2]-values[-3]
args=sys.argv[sys.argv.index('--')+1:]
pathlib.Path(args[0]).write_text(json.dumps(dict(schema='synthetic-wedge-angle-lut.v1',blender=bpy.app.version_string,steps=N,values=values,source='newly constructed analytic two-triangle wedges; measured normal ratios'),indent=2))
print('generated',len(values),'synthetic observations')
