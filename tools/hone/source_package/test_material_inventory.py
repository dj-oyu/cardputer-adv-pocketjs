"""Synthetic Blender node/link inventory contract."""
import bpy, importlib.util, pathlib, json
spec=importlib.util.spec_from_file_location('material_inventory',pathlib.Path(__file__).with_name('material_inventory.py'))
mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
bpy.ops.wm.read_factory_settings(use_empty=True)
mesh=bpy.data.meshes.new('mesh');mesh.from_pydata([(0,0,0),(1,0,0),(0,1,0)],[],[(0,1,2)])
obj=bpy.data.objects.new('object',mesh);bpy.context.collection.objects.link(obj)
mat=bpy.data.materials.new('synthetic');mat.use_nodes=True;mesh.materials.append(mat)
tree=mat.node_tree;math=tree.nodes.new('ShaderNodeMath');math.operation='MULTIPLY';math.inputs[1].default_value=.25
ramp=tree.nodes.new('ShaderNodeValToRGB');ramp.color_ramp.elements[0].position=.1
tree.links.new(math.outputs[0],ramp.inputs[0])
records=json.loads(json.dumps(mod.inventory(),allow_nan=False));assert len(records)==1
nodes={n['name']:n for n in records[0]['nodes']};m=nodes[math.name];r=nodes[ramp.name]
assert m['operation']=='MULTIPLY' and m['inputs'][1]['default']['value']==.25
assert abs(r['color_ramp']['elements'][0]['position']-.1)<1e-6
links=[l for l in records[0]['links'] if l['source_node']==math.name and l['target_node']==ramp.name]
assert len(links)==1 and links[0]['source_socket']==0 and links[0]['target_socket']==0
print('PASS material node, duplicate socket index, link and ramp inventory')
