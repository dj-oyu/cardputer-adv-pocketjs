"""Generate an editable synthetic curved grid and real Blender GLB export."""
import json
from pathlib import Path
import sys
import bpy

out = Path(sys.argv[sys.argv.index('--') + 1]).resolve()
out.mkdir(parents=True, exist_ok=False)
bpy.ops.object.select_all(action='SELECT')
bpy.ops.object.delete(use_global=False)
vertices = []
for y in range(9):
    for x in range(9):
        u, v = x / 4 - 1, y / 4 - 1
        # Blender Z-up; GLB converts to Y-up. The shape remains an open patch.
        vertices.append((u, v, .1 * (1-u*u) * (1-v*v)))
faces = []
for y in range(8):
    for x in range(8):
        a = y * 9 + x
        faces.extend([(a, a+1, a+10), (a, a+10, a+9)])
mesh = bpy.data.meshes.new('editable-synthetic-grid')
mesh.from_pydata(vertices, [], faces)
mesh.update()
obj = bpy.data.objects.new('synthetic-grid-not-fox', mesh)
bpy.context.collection.objects.link(obj)
bpy.context.view_layer.objects.active = obj
obj.select_set(True)
for polygon in mesh.polygons:
    polygon.use_smooth = True
mat = bpy.data.materials.new('explicit-opaque')
mat.use_nodes = True
shader = mat.node_tree.nodes.get('Principled BSDF')
shader.inputs['Base Color'].default_value = (1, .5, .1, 1)
shader.inputs['Metallic'].default_value = 0
shader.inputs['Roughness'].default_value = .8
mat.use_backface_culling = True
obj.data.materials.append(mat)
# Capture authored mesh normals before the exporter runs, not from GLB.
source_normals = [{"vertex": loop.vertex_index, "normal": list(mesh.corner_normals[loop.index].vector)}
                  for loop in mesh.loops]
bpy.ops.wm.save_as_mainfile(filepath=str(out / 'source.blend'))
bpy.ops.export_scene.gltf(filepath=str(out / 'source.glb'), export_format='GLB',
                          use_selection=True, export_animations=False,
                          export_cameras=False, export_lights=False,
                          export_texcoords=False, export_normals=True,
                          export_yup=True)
(out / 'blender-source.json').write_text(json.dumps({
    'scope': 'SYNTHETIC GRID, NOT PRIVATE ART OR FOX ACCEPTANCE',
    'blenderVersion': bpy.app.version_string, 'vertices': vertices, 'faces': faces,
    'cornerNormals': source_normals,
    'units': 'Blender scene units', 'exportYUp': True}) + '\n')
