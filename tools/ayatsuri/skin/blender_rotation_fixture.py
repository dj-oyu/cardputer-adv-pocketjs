"""Authored public six-vertex two-bone skin fixture; build-time receipt only."""
import bpy,json,sys,math
from pathlib import Path
out=Path(sys.argv[sys.argv.index('--')+1]).resolve();out.mkdir(parents=True,exist_ok=False)
bpy.ops.object.select_all(action='SELECT');bpy.ops.object.delete(use_global=False)
verts=[(x,0,-y) for y in (-15,15) for x in (-30,0,30)]
faces=[(0,4,1),(0,3,4),(1,5,2),(1,4,5)]
mesh=bpy.data.meshes.new('public-strip');mesh.from_pydata(verts,[],faces);mesh.update()
obj=bpy.data.objects.new('public-strip',mesh);bpy.context.collection.objects.link(obj)
arm=bpy.data.armatures.new('public-two-bones');rig=bpy.data.objects.new('public-two-bones',arm);bpy.context.collection.objects.link(rig)
bpy.context.view_layer.objects.active=rig;rig.select_set(True);bpy.ops.object.mode_set(mode='EDIT')
r=arm.edit_bones.new('root');r.head=(0,0,0);r.tail=(0,10,0)
c=arm.edit_bones.new('child');c.head=(15,0,0);c.tail=(15,10,0);c.parent=r;c.use_connect=False
bpy.ops.object.mode_set(mode='OBJECT')
weights=[]
for name in ('root','child'):obj.vertex_groups.new(name=name)
for i in range(6):
 w=(0,1/3,1)[i%3];weights.append([1-w,w])
 for j,value in enumerate(weights[-1]):
  if value:obj.vertex_groups[j].add([i],value,'REPLACE')
mod=obj.modifiers.new('only-armature','ARMATURE');mod.object=rig;mod.use_deform_preserve_volume=False
bpy.context.view_layer.update()
assert all(abs(obj.matrix_world[i][j]-(1 if i==j else 0))<1e-8 and abs(rig.matrix_world[i][j]-(1 if i==j else 0))<1e-8 for i in range(4) for j in range(4))
assert len(obj.modifiers)==1 and not obj.data.shape_keys
for loop in mesh.loops:
 assert (mesh.corner_normals[loop.index].vector-mesh.vertices[loop.vertex_index].normal).length<1e-6
weights=[[next((g.weight for g in v.groups if g.group==j),0.0) for j in range(2)] for v in mesh.vertices]
mat=lambda m:[[float(x) for x in row] for row in m]
receipt={'schema':'synthetic-blender-skin-rotation-v1','blender':bpy.app.version_string,'vertices':verts,'faces':faces,'normals':[list(v.normal) for v in mesh.vertices],'weights':weights,'restMatrices':[mat(rig.matrix_world@arm.bones[n].matrix_local) for n in ('root','child')],'samples':[],'authoring':{'scope':'fixed-public-strip-only','objectTransforms':'identity','bones':['root','child'],'modifiers':['ARMATURE'],'constraints':sum(len(b.constraints) for b in rig.pose.bones),'drivers':len(rig.animation_data.drivers) if rig.animation_data else 0,'shapeKeys':bool(mesh.shape_keys)}}
for name,offset,angle in [('rest',0,0),('child_y6',6,0),('child_rx30',0,math.pi/6)]:
 rig.pose.bones['child'].rotation_mode='XYZ';rig.pose.bones['child'].rotation_euler=(angle,0,0)
 rig.pose.bones['child'].location=(0,offset,0);bpy.context.view_layer.update()
 evaluated=obj.evaluated_get(bpy.context.evaluated_depsgraph_get());m=evaluated.to_mesh()
 receipt['samples'].append({'name':name,'matrices':[mat(rig.matrix_world@rig.pose.bones[n].matrix) for n in ('root','child')],'positions':[list(v.co) for v in m.vertices],'normals':[list(v.normal) for v in m.vertices]});evaluated.to_mesh_clear()
rig.pose.bones['child'].rotation_euler=(0,0,0);rig.pose.bones['child'].location=(0,0,0);bpy.context.view_layer.update()
bpy.ops.wm.save_as_mainfile(filepath=str(out/'source.blend'))
(out/'receipt.json').write_text(json.dumps(receipt)+'\n')
