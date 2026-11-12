"""New public-only DQ fixture; no source model or private data is loaded."""
import bpy,sys,json,math
from pathlib import Path
from mathutils import Quaternion,Vector
out=Path(sys.argv[sys.argv.index('--')+1]);out.mkdir(parents=True,exist_ok=False)
bpy.ops.object.select_all(action='SELECT');bpy.ops.object.delete(use_global=False)
vertices=[(x,y,0) for y in (-1,1) for x in (-2,0,2)];faces=[(0,1,4),(0,4,3),(1,2,5),(1,5,4)]
m=bpy.data.meshes.new('public-grid');m.from_pydata(vertices,[],faces);m.update();obj=bpy.data.objects.new('public-grid',m);bpy.context.collection.objects.link(obj)
a=bpy.data.armatures.new('public-rig');rig=bpy.data.objects.new('public-rig',a);bpy.context.collection.objects.link(rig);bpy.context.view_layer.objects.active=rig;rig.select_set(True);bpy.ops.object.mode_set(mode='EDIT')
r=a.edit_bones.new('root');r.head=(0,0,0);r.tail=(0,1,0)
c=a.edit_bones.new('child');c.head=(1,0,0);c.tail=(1,1,0);c.parent=r;c.use_connect=False
bpy.ops.object.mode_set(mode='OBJECT')
for n in ('root','child'):obj.vertex_groups.new(name=n)
for i in range(6):
 w=(0,.5,1)[i%3]
 for j,v in enumerate((1-w,w)):
  if v:obj.vertex_groups[j].add([i],v,'REPLACE')
mod=obj.modifiers.new('public-armature','ARMATURE');mod.object=rig
rig.pose.bones['child'].rotation_mode='QUATERNION';rig.pose.bones['child'].rotation_quaternion=Quaternion(Vector((1,0,1)).normalized(),math.pi/3)
bpy.context.view_layer.update()
receipt={'schema':'public-dq-fixture-v1','blender':bpy.app.version_string,'vertices':[list(v.co) for v in m.vertices],'faces':[list(p.vertices) for p in m.polygons],'weights':[[next((g.weight for g in v.groups if g.group==j),0) for j in range(2)] for v in m.vertices],'samples':{}}
for mode in ('LBS','DQ'):
 mod.use_deform_preserve_volume=mode=='DQ';bpy.context.view_layer.update();ev=obj.evaluated_get(bpy.context.evaluated_depsgraph_get());mesh=ev.to_mesh()
 receipt['samples'][mode]={'positions':[list(v.co) for v in mesh.vertices],'faceNormals':[list(p.normal) for p in mesh.polygons]};ev.to_mesh_clear()
(out/'receipt.json').write_text(json.dumps(receipt,sort_keys=True)+'\n');bpy.ops.wm.save_as_mainfile(filepath=str(out/'source.blend'))
