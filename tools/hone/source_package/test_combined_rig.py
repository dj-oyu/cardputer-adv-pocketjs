"""Blender synthetic combined relative morph + single-bone skin oracle."""
import bpy,math
from mathutils import Vector
bpy.ops.wm.read_factory_settings(use_empty=True)
mesh=bpy.data.meshes.new('quad');pts=[(0,0,0),(1,0,0),(1,1,0),(0,1,0)];mesh.from_pydata(pts,[],[(0,1,2,3)])
o=bpy.data.objects.new('synthetic',mesh);bpy.context.collection.objects.link(o)
basis=o.shape_key_add(name='Basis');a=o.shape_key_add(name='Smile');b=o.shape_key_add(name='Blink')
for v in a.data:v.co.x+=.2
for v in b.data:v.co.z+=.3
group=o.vertex_groups.new(name='Bone');group.add(list(range(4)),1.,'REPLACE')
arm=bpy.data.armatures.new('rig');rig=bpy.data.objects.new('rig',arm);bpy.context.collection.objects.link(rig);bpy.context.view_layer.objects.active=rig;rig.select_set(True)
bpy.ops.object.mode_set(mode='EDIT');bone=arm.edit_bones.new('Bone');bone.head=(0,0,0);bone.tail=(0,0,1);bpy.ops.object.mode_set(mode='OBJECT')
mod=o.modifiers.new('skin','ARMATURE');mod.object=rig;mod.use_deform_preserve_volume=True
pose=rig.pose.bones['Bone'];pose.rotation_mode='XYZ'
maximum=0
for wa,wb,angle in [(0,0,0),(.25,.5,.2),(1,.001,-.3),(.5,1,.4)]:
 a.value=wa;b.value=wb;pose.rotation_euler.z=angle;o.update_tag();rig.update_tag();bpy.context.view_layer.update();dg=bpy.context.evaluated_depsgraph_get();ev=o.evaluated_get(dg);m=ev.to_mesh()
 transform=rig.evaluated_get(dg).pose.bones['Bone'].matrix @ arm.bones['Bone'].matrix_local.inverted()
 for i,p in enumerate(pts):
  expected=transform @ (Vector(p)+Vector((.2*wa,0,.3*wb)));maximum=max(maximum,(m.vertices[i].co-expected).length)
 assert len(m.vertices)==4 and len(m.polygons)==1
 ev.to_mesh_clear()
assert maximum<1e-6,maximum
print('PASS four combined morph+rig synthetic poses; max position error',maximum,'not full private-rig admission')
