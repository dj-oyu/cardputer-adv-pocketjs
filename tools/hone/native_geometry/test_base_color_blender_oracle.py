"""Render sampled base-color inputs via Blender Cycles; no lighting claim.

Run against an already opened trusted blend with --disable-autoexec. DLLs must
be built from the same inventory and routes; verify their identity externally.
Only aggregate errors are printed. Temporary render files contain source data.
"""
import bpy,ctypes as c,hashlib,json,pathlib,sys,tempfile

args=sys.argv[sys.argv.index('--')+1:]
if len(args)!=6:raise SystemExit('expected inventory, material routes, source package, driver bindings, driver DLL, color DLL')
inventory,routes,package,bindings=(json.loads(pathlib.Path(x).read_text(encoding='utf8')) for x in args[:4])
source=pathlib.Path(bpy.data.filepath);digest=hashlib.sha256(source.read_bytes()).hexdigest()
if any(x.get('source_sha256')!=digest or x.get('runtime_ready') is not False for x in (inventory,routes,package,bindings)):
    raise ValueError('source identity')
driver=c.CDLL(str(pathlib.Path(args[4]).resolve())).run_frame
driver.argtypes=[c.POINTER(c.c_double),c.c_size_t,c.POINTER(c.c_double),c.POINTER(c.c_double)]
evaluate=c.CDLL(str(pathlib.Path(args[5]).resolve())).base_color
evaluate.argtypes=[c.c_uint,c.POINTER(c.c_float),c.POINTER(c.c_double),c.c_size_t,c.POINTER(c.c_float)]
scene=bpy.data.scenes.new('diagnostic-color-oracle');scene.render.engine='CYCLES';scene.cycles.samples=1;scene.cycles.use_adaptive_sampling=False;scene.cycles.use_denoising=False
scene.render.resolution_x=1;scene.render.resolution_y=1;scene.render.resolution_percentage=100;scene.render.film_transparent=False
scene.view_settings.view_transform='Standard';scene.render.filter_size=.01
scene.render.image_settings.file_format='OPEN_EXR';scene.render.image_settings.color_mode='RGBA';scene.render.image_settings.color_depth='32'
camera_data=bpy.data.cameras.new('diagnostic-camera');camera_data.type='ORTHO';camera_data.ortho_scale=.00001
camera=bpy.data.objects.new('diagnostic-camera',camera_data);scene.collection.objects.link(camera);scene.camera=camera
mesh=bpy.data.meshes.new('diagnostic-plane');obj=bpy.data.objects.new('diagnostic-plane',mesh);scene.collection.objects.link(obj)
cases=0;worst=0.;errors=0;non_grey=0
with tempfile.TemporaryDirectory() as tmp:
 scene.render.filepath=str(pathlib.Path(tmp)/'color-oracle.exr')
 for mid,record in enumerate(inventory['materials']):
  output=next(n for n in record['nodes'] if n['type']=='ShaderNodeOutputMaterial' and n['active_output'])
  principal=next(n for n in record['nodes'] if n['type']=='ShaderNodeBsdfPrincipled')
  base_socket=next(s for s in principal['inputs'] if s['name']=='Base Color')
  source_mesh=next(m for m in package['meshes'] if record['name'] in m['material_slots']);vertices=source_mesh['vertices'];n=len(vertices)
  if not n:raise ValueError('empty sampled mesh')
  samples=[tuple(map(float,vertices[i])) for i in (0,n//3,(2*n)//3,n-1)]+[(100.,0.,100.)]
  for control in (0.,.003,.01,.019,1.):
   inputs=(c.c_double*len(bindings['inputs']))(*([control]*len(bindings['inputs'])));scratch=(c.c_double*len(bindings['programs']))();values=(c.c_double*len(bindings['programs']))()
   if not driver(inputs,len(inputs),scratch,values):raise ValueError('driver evaluation')
   material=bpy.data.materials[record['name']].copy();tree=material.node_tree
   if tree.animation_data:tree.animation_data_clear()
   for route in routes['shader_routes']:
    if route['material']!=record['name']:continue
    socket=getattr(tree.nodes[route['node']],route['direction'])[route['socket']]
    if isinstance(socket.default_value,float):socket.default_value=values[route['program']]
    else:socket.default_value[route['component']]=values[route['program']]
   body=tree.nodes[principal['name']];socket=body.inputs[base_socket['index']];emission=tree.nodes.new('ShaderNodeEmission')
   linked=next((l.from_socket for l in tree.links if l.to_node==body and l.to_socket==socket),None)
   if linked is None:emission.inputs['Color'].default_value=socket.default_value
   else:tree.links.new(linked,emission.inputs['Color'])
   tree.links.new(emission.outputs[0],tree.nodes[output['name']].inputs['Surface'])
   mesh.materials.clear();mesh.materials.append(material)
   for xyz in samples:
    x,y,z=xyz;epsilon=.01;mesh.clear_geometry();mesh.from_pydata([(x-epsilon,y-epsilon,z),(x+epsilon,y-epsilon,z),(x+epsilon,y+epsilon,z),(x-epsilon,y+epsilon,z)],[],[(0,1,2,3)]);mesh.update()
    camera.location=(x,y,z+1);camera.rotation_euler=(0,0,0)
    native=(c.c_float*3)(*xyz);result=(c.c_float*3)()
    if not evaluate(mid,native,values,len(values),result):raise ValueError('base color evaluation')
    bpy.ops.render.render(scene=scene.name,write_still=True);image=bpy.data.images.load(scene.render.filepath,check_existing=False);pixels=image.pixels[:];bpy.data.images.remove(image)
    if len(pixels)<4:raise ValueError('missing render sample')
    delta=max(abs(pixels[k]-result[k]) for k in range(3));worst=max(worst,delta);errors+=delta>1e-3;cases+=1
    non_grey+=max(result)-min(result)>1e-3
   mesh.materials.clear();bpy.data.materials.remove(material)
print('BASE_COLOR_RENDER_COMPARE cases',cases,'max_component_error',worst,'over_1e-3',errors,'non_grey_cases',non_grey,'lighting_and_compositing=unverified','runtime_ready',False)
if errors or not cases:raise SystemExit('base color mismatch or no sampled materials')
