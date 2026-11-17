"""Render sampled alpha factors through Blender Cycles; no full-model claim.

Run against an already opened trusted blend with --disable-autoexec. Input
headers/DLLs must be built from the same inventory and routes; their identity
must be checked externally. Only aggregate numeric results are printed.
"""
import bpy,ctypes as c,hashlib,json,pathlib,random,sys,tempfile

args=sys.argv[sys.argv.index('--')+1:]
if len(args)!=6:raise SystemExit('expected inventory, material routes, source package, driver bindings, driver DLL, alpha DLL')
inventory,routes,package,bindings=(json.loads(pathlib.Path(x).read_text(encoding='utf8')) for x in args[:4])
source=pathlib.Path(bpy.data.filepath);digest=hashlib.sha256(source.read_bytes()).hexdigest()
if any(x.get('source_sha256')!=digest or x.get('runtime_ready') is not False for x in (inventory,routes,package,bindings)):
    raise ValueError('source identity')
driver=c.CDLL(str(pathlib.Path(args[4]).resolve())).run_frame
driver.argtypes=[c.POINTER(c.c_double),c.c_size_t,c.POINTER(c.c_double),c.POINTER(c.c_double)]
evaluate=c.CDLL(str(pathlib.Path(args[5]).resolve())).alpha_factor
evaluate.argtypes=[c.c_uint,c.POINTER(c.c_float),c.POINTER(c.c_double),c.c_size_t,c.POINTER(c.c_float)]
scene=bpy.data.scenes.new('diagnostic-alpha-oracle');scene.render.engine='CYCLES';scene.cycles.samples=1;scene.cycles.use_adaptive_sampling=False;scene.cycles.use_denoising=False
scene.render.resolution_x=1;scene.render.resolution_y=1;scene.render.resolution_percentage=100;scene.render.film_transparent=False
scene.view_settings.view_transform='Standard';scene.render.filter_size=.01
scene.render.image_settings.file_format='OPEN_EXR';scene.render.image_settings.color_mode='RGBA';scene.render.image_settings.color_depth='32'
camera_data=bpy.data.cameras.new('diagnostic-camera');camera_data.type='ORTHO';camera_data.ortho_scale=.00001
camera=bpy.data.objects.new('diagnostic-camera',camera_data);scene.collection.objects.link(camera);scene.camera=camera
mesh=bpy.data.meshes.new('diagnostic-plane');obj=bpy.data.objects.new('diagnostic-plane',mesh);scene.collection.objects.link(obj)
cases=0;worst=0.;errors=0;fractional=0;low=1.;high=0.;observed={}
rng=random.Random(6405)
controls=[[v]*len(bindings['inputs']) for v in (0.,.003,.01,.019,1.)]
controls += [[rng.random() for _ in bindings['inputs']] for _ in range(8)]
with tempfile.TemporaryDirectory() as tmp:
 scene.render.filepath=str(pathlib.Path(tmp)/'alpha-oracle.exr')
 for mid,record in enumerate(inventory['materials']):
  nodes={n['name']:n for n in record['nodes']};links=[l for l in record['links'] if not l['muted']]
  out=next(n for n in record['nodes'] if n['type']=='ShaderNodeOutputMaterial' and n['active_output'])
  surface=next((l for l in links if l['target_node']==out['name'] and l['target_socket']==0),None)
  if not surface or nodes[surface['source_node']]['type']!='ShaderNodeMixShader':continue
  source_mesh=next(m for m in package['meshes'] if record['name'] in m['material_slots']);vertices=source_mesh['vertices'];n=len(vertices)
  if not n:raise ValueError('empty sampled mesh')
  samples=[tuple(map(float,vertices[i])) for i in (0,n//3,(2*n)//3,n-1)]+[(100.,0.,100.)]
  for control in controls:
   inputs=(c.c_double*len(bindings['inputs']))(*control);scratch=(c.c_double*len(bindings['programs']))();values=(c.c_double*len(bindings['programs']))()
   if not driver(inputs,len(inputs),scratch,values):raise ValueError('driver evaluation')
   material=bpy.data.materials[record['name']].copy();tree=material.node_tree
   if tree.animation_data:tree.animation_data_clear()
   for route in routes['shader_routes']:
    if route['material']!=record['name']:continue
    socket=getattr(tree.nodes[route['node']],route['direction'])[route['socket']]
    if isinstance(socket.default_value,float):socket.default_value=values[route['program']]
    else:socket.default_value[route['component']]=values[route['program']]
   output=tree.nodes[out['name']];mix=tree.nodes[surface['source_node']]
   factor=next((l.from_socket for l in tree.links if l.to_node==mix and l.to_socket==mix.inputs[0]),None)
   if factor is None:raise ValueError('unlinked mix factor')
   emission=tree.nodes.new('ShaderNodeEmission');tree.links.new(factor,emission.inputs['Color']);tree.links.new(emission.outputs[0],output.inputs['Surface'])
   mesh.materials.clear();mesh.materials.append(material)
   for sample_index,xyz in enumerate(samples):
    x,y,z=xyz;epsilon=.01;mesh.clear_geometry();mesh.from_pydata([(x-epsilon,y-epsilon,z),(x+epsilon,y-epsilon,z),(x+epsilon,y+epsilon,z),(x-epsilon,y+epsilon,z)],[],[(0,1,2,3)]);mesh.update()
    camera.location=(x,y,z+1);camera.rotation_euler=(0,0,0)
    native=(c.c_float*3)(*xyz);result=c.c_float()
    if not evaluate(mid,native,values,len(values),c.byref(result)):raise ValueError('alpha evaluation')
    bpy.ops.render.render(scene=scene.name,write_still=True);image=bpy.data.images.load(scene.render.filepath,check_existing=False);pixels=image.pixels[:];bpy.data.images.remove(image)
    if len(pixels)<4:raise ValueError('missing render sample')
    delta=abs(pixels[0]-result.value);worst=max(worst,delta);errors+=delta>1e-3;cases+=1
    fractional+=0.001<result.value<.999;low=min(low,result.value);high=max(high,result.value)
    observed.setdefault((mid,sample_index),[]).append(result.value)
   mesh.materials.clear();bpy.data.materials.remove(material)
changing=sum(max(v)-min(v)>1e-4 for v in observed.values())
print('ALPHA_GRAPH_RENDER_COMPARE cases',cases,'max_component_error',worst,'over_1e-3',errors,'fractional_cases',fractional,'varying_points',changing,'range',low,high,'colors_and_full_render=unverified','runtime_ready',False)
if errors or not cases:raise SystemExit('alpha graph mismatch or no sampled materials')
