"""Public synthetic split/custom corner-normal cases; no user artwork."""
import bpy,json,sys
from pathlib import Path
out=Path(sys.argv[sys.argv.index('--')+1]).resolve();out.mkdir(parents=True,exist_ok=False)
for case in ('flat','custom'):
 bpy.ops.object.select_all(action='SELECT');bpy.ops.object.delete(use_global=False)
 vertices=[(0,0,0),(1,0,0),(0,1,0),(0,0,1)] if case=='flat' else [(0,0,0),(1,0,0),(0,1,0),(1,1,0)]
 faces=[(0,1,2),(1,0,3)] if case=='flat' else [(0,1,2),(1,3,2)]
 mesh=bpy.data.meshes.new(case);mesh.from_pydata(vertices,[],faces);mesh.update()
 obj=bpy.data.objects.new(case,mesh);bpy.context.collection.objects.link(obj);obj.select_set(True);bpy.context.view_layer.objects.active=obj
 for p in mesh.polygons:p.use_smooth=case=='custom'
 if case=='custom':mesh.normals_split_custom_set([(0,0,1)]*3+[(.6,0,.8)]*3)
 mat=bpy.data.materials.new(case);mat.use_nodes=True;mat.use_backface_culling=True;mesh.materials.append(mat)
 corners=[{'vertex':l.vertex_index,'normal':list(mesh.corner_normals[l.index].vector)} for l in mesh.loops]
 (out/(case+'.json')).write_text(json.dumps({'case':case,'vertices':vertices,'faces':faces,'corners':corners,'version':bpy.app.version_string}))
 bpy.ops.export_scene.gltf(filepath=str(out/(case+'.glb')),export_format='GLB',use_selection=True,export_animations=False,export_cameras=False,export_lights=False,export_texcoords=False,export_normals=True,export_yup=True)
