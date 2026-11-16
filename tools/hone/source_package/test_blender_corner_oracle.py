import pathlib,sys,bpy
sys.path.insert(0,str(pathlib.Path(__file__).parent))
from blender_corner_oracle import evaluate
m=dict(vertices=[[0,0,0],[1,0,0],[1,0,0],[0,0,0]],polygons=[dict(vertices=[0,1,2,3],smooth=True)],triangles=[dict(polygon=0,vertices=[0,1,2],loops=[0,1,2]),dict(polygon=0,vertices=[0,2,3],loops=[0,2,3])])
# Obtain Blender's fixed diagonal on this synthetic collapsed quad.
t=bpy.data.meshes.new('synthetic-topology');t.from_pydata(m['vertices'],[],[[0,1,2,3]]);t.calc_loop_triangles();m['triangles']=[dict(polygon=x.polygon_index,vertices=list(x.vertices),loops=list(x.loops)) for x in t.loop_triangles];bpy.data.meshes.remove(t)
r=evaluate(m,m['vertices']);assert len(r['zero_area_triangles'])==2;assert r['corner_normals']==[[0.,0.,1.]]*4
r=evaluate(m,[[0,0,0],[1,0,0],[1,1,0],[0,1,0]]);assert not r['zero_area_triangles'];assert r['corner_normals']==[[0.,0.,1.]]*4
assert r['triangles']==m['triangles'];assert r['runtime_ready'] is False
print('PASS collapsed/open quad: Blender fallback unit normals, topology preservation, explicit non-runtime oracle')
