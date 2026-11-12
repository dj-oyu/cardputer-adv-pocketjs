"""Public tilted triangle with explicit object TRS; isolated transform contract."""
import bpy, json, math, sys
from pathlib import Path
out=Path(sys.argv[sys.argv.index('--')+1]);out.mkdir(parents=True,exist_ok=False)
vertices=[(0,0,0),(2,-1,0),(3,0,-1)] # cross=(1,2,3), analytic independent normal
for name,scale in [('unit_scale',(1,1,1)),('nonuniform',(2,.5,1.5)),('mirrored',(-2,.5,1.5)),('double_mirror',(-2,-.5,1.5))]:
 bpy.ops.object.select_all(action='SELECT');bpy.ops.object.delete(use_global=False)
 mesh=bpy.data.meshes.new(name);mesh.from_pydata(vertices,[],[(0,1,2)]);mesh.update()
 obj=bpy.data.objects.new(name,mesh);bpy.context.collection.objects.link(obj);bpy.context.view_layer.objects.active=obj;obj.select_set(True)
 obj.scale=scale;obj.rotation_euler=(0,0,math.pi/6);obj.location=(.25,-.5,.75)
 mat=bpy.data.materials.new('opaque');mat.use_nodes=True;mat.use_backface_culling=True;obj.data.materials.append(mat)
 bpy.context.view_layer.update()
 bpy.ops.export_scene.gltf(filepath=str(out/(name+'.glb')),export_format='GLB',use_selection=True,export_animations=False,export_cameras=False,export_lights=False,export_texcoords=False,export_normals=True,export_yup=True)
 (out/(name+'.json')).write_text(json.dumps({'name':name,'scale':scale,'rotationZ':math.pi/6,'translation':[.25,-.5,.75],'vertices':vertices,'faces':[[0,1,2]],'cornerNormals':[list(mesh.corner_normals[l.index].vector) for l in mesh.loops],'version':bpy.app.version_string}))
