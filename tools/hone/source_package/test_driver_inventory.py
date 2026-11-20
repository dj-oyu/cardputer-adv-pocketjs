"""Synthetic Blender-only driver inventory contract."""
import bpy, importlib.util, pathlib

spec=importlib.util.spec_from_file_location('driver_inventory',pathlib.Path(__file__).with_name('driver_inventory.py'))
mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
bpy.ops.wm.read_factory_settings(use_empty=True)
obj=bpy.data.objects.new('control',None);bpy.context.collection.objects.link(obj);obj['value']=.25
curve=obj.driver_add('location',0);driver=curve.driver;driver.expression='x*2'
variable=driver.variables.new();variable.name='x';variable.type='SINGLE_PROP'
variable.targets[0].id_type='OBJECT';variable.targets[0].id=obj;variable.targets[0].data_path='["value"]'
material=bpy.data.materials.new('synthetic');material.use_nodes=True
node=material.node_tree.nodes.get('Principled BSDF')
curve=node.inputs['Roughness'].driver_add('default_value');curve.driver.expression='y**2'
variable=curve.driver.variables.new();variable.name='y';variable.type='SINGLE_PROP'
variable.targets[0].id_type='OBJECT';variable.targets[0].id=obj;variable.targets[0].data_path='["value"]'
records=mod.inventory();assert len(records)==2
assert {r['owner']['type'] for r in records}=={'Object','ShaderNodeTree'}
tree=next(r for r in records if r['owner']['type']=='ShaderNodeTree')
assert tree['owner']['embedded_in']==[{'type':'Material','name':'synthetic'}]
for r in records:
    t=r['variables'][0]['targets'][0]
    assert t['id']=={'type':'Object','name':'control'} and t['current']=={'status':'ok','value':.25}
    assert r['curve_valid'] and r['driver_valid'] and r['keyframe_count']==0
    assert len(r['fcurve_modifiers'])==1 and r['fcurve_modifiers'][0]['coefficients']==[0.,1.]
print('PASS object and material node-tree driver inventory')
