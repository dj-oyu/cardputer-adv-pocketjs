"""Synthetic plane oracle plus preserved modifier contract; no native wrap claim."""
import bpy,json,sys,pathlib
sys.path.insert(0,str(pathlib.Path(__file__).parent))
from export_package import modifier_record
bpy.ops.wm.read_factory_settings(use_empty=True)
def mesh(name,coords,faces):
 m=bpy.data.meshes.new(name);m.from_pydata(coords,[],faces);o=bpy.data.objects.new(name,m);bpy.context.collection.objects.link(o);return o
target=mesh('target',[(-3,-3,0),(3,-3,0),(3,3,0),(-3,3,0)],[(0,1,2,3)])
source=mesh('source',[(-1,-1,.5),(1,-1,.5),(1,1,.5),(-1,1,.5)],[(0,1,2,3)])
b=source.shape_key_add(name='Basis');k=source.shape_key_add(name='Bend')
for v in k.data:v.co.z+=.3;v.co.x+=.2
m=source.modifiers.new('nearest','SHRINKWRAP');m.target=target;m.wrap_method='NEAREST_SURFACEPOINT';m.wrap_mode='ON_SURFACE';m.offset=.125
record=modifier_record(m);assert record['shrinkwrap']['target']=='target' and record['shrinkwrap']['offset']==.125;assert json.loads(json.dumps(record))==record
for weight in (0,.001,.5,1):
 k.value=weight;source.update_tag();bpy.context.view_layer.update();ev=source.evaluated_get(bpy.context.evaluated_depsgraph_get());out=ev.to_mesh()
 for i,v in enumerate(out.vertices):
  assert abs(v.co.z-.125)<1e-6
  assert abs(v.co.x-(b.data[i].co.x+.2*weight))<1e-6
 ev.to_mesh_clear()
assert len(source.modifiers)==1 and source.data.vertices[0].co.z==.5
print('PASS shrinkwrap settings JSON and morph-before-wrap 4 flat-plane samples; curved/moving-target native consumer unsupported')
