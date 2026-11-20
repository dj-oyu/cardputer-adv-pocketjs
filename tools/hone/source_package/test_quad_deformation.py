"""Synthetic Blender oracle: preserve loop topology across a collapsed morph."""
import bpy
bpy.ops.wm.read_factory_settings(use_empty=True)
m=bpy.data.meshes.new('collapsed-quad')
m.from_pydata([(0,0,0),(1,0,0),(1,0,0),(0,0,0)],[],[(0,1,2,3)])
m.polygons[0].use_smooth=True;m.calc_loop_triangles();base=[tuple(t.loops) for t in m.loop_triangles]
assert len(base)==2
# Never discard rest-degenerate topology: the expression opens this surface.
for w in [0.,.25,.5,.75,1.]:
 for v,co in zip(m.vertices,[(0,0,0),(1,0,0),(1,w,.1*w),(0,w,0)]):v.co=co
 m.update();m.calc_loop_triangles()
 assert len(m.loop_triangles)==2
 for t in m.loop_triangles:
  assert all(m.loops[li].vertex_index==vi for li,vi in zip(t.loops,t.vertices))
 if w==1:assert sum(t.area for t in m.loop_triangles)>.9
print('PASS collapsed-to-open quad morph: loop correspondence and triangles preserved; no normal-equivalence claim')
