import bpy,importlib.util,pathlib,json,copy
s=importlib.util.spec_from_file_location('export_package',pathlib.Path(__file__).with_name('export_package.py'));x=importlib.util.module_from_spec(s);s.loader.exec_module(x)
bpy.ops.wm.read_factory_settings(use_empty=True)
m=bpy.data.meshes.new('mixed');m.from_pydata([(0,0,0),(1,0,0),(0,1,0),(0,0,1)],[],[(0,1,2),(0,3,1)])
o=bpy.data.objects.new('synthetic',m);bpy.context.collection.objects.link(o)
m.polygons[0].use_smooth=False;m.polygons[1].use_smooth=True
basis=o.shape_key_add(name='Basis');key=o.shape_key_add(name='Blink');key.data[3].co.z=.5
r=x.mesh_record(o);p=dict(schema='hone.source-preservation.v1',runtime_ready=False,meshes=[r]);x.validate(p)
q=json.loads(json.dumps(p));assert p==q
assert [f['smooth'] for f in q['meshes'][0]['polygons']]==[False,True]
assert q['meshes'][0]['shape_keys']['blocks'][1]['positions'][3][2]==.5
bad=copy.deepcopy(p);bad['meshes'][0]['shape_keys']['blocks'][1]['positions'].pop()
try:x.validate(bad)
except AssertionError:pass
else:raise AssertionError('Must reject missing morph vertex')
bad=copy.deepcopy(p);bad['meshes'][0]['triangles'][0]['loops'][0]=9999
try:x.validate(bad)
except AssertionError:pass
else:raise AssertionError('Must reject missing corner normal')
print('PASS mixed-flat/smooth, morph roundtrip, malformed morph and corner rejection')
