"""Blender source-preservation intermediate, NOT a runtime-ready firmware asset."""
import bpy, hashlib, json, pathlib, sys, math

def vec(v): return list(v)
def matrix(m): return [list(row) for row in m]
def mesh_record(o):
 m=o.data;m.calc_loop_triangles()
 keys=m.shape_keys
 return dict(name=o.name, parent=o.parent.name if o.parent else None,parent_type=o.parent_type,parent_bone=o.parent_bone,matrix_world=matrix(o.matrix_world),matrix_parent_inverse=matrix(o.matrix_parent_inverse),vertices=[vec(v.co) for v in m.vertices], polygons=[dict(vertices=list(p.vertices),loops=list(p.loop_indices),smooth=p.use_smooth,material=p.material_index) for p in m.polygons],triangles=[dict(vertices=list(t.vertices),loops=list(t.loops),polygon=t.polygon_index) for t in m.loop_triangles],corner_normals=[vec(n.vector) for n in m.corner_normals],normal_policy='preserve_polygon_and_corner_modes',has_custom_normals=m.has_custom_normals,sharp_edges=[i for i,e in enumerate(m.edges) if e.use_edge_sharp],uv_layers=[dict(name=l.name,uv=[vec(x.uv) for x in l.data]) for l in m.uv_layers],vertex_groups=[g.name for g in o.vertex_groups],weights=[[[g.group,g.weight] for g in v.groups] for v in m.vertices],shape_keys=None if not keys else dict(use_relative=keys.use_relative,eval_time=keys.eval_time,blocks=[dict(name=k.name,relative_key=k.relative_key.name,vertex_group=k.vertex_group,value=k.value,slider_min=k.slider_min,slider_max=k.slider_max,mute=k.mute,interpolation=k.interpolation,frame=k.frame,positions=[vec(v.co) for v in k.data]) for k in keys.key_blocks]),modifiers=[dict(type=x.type,name=x.name,viewport=x.show_viewport,render=x.show_render,object=getattr(getattr(x,'object',None),'name',None),preserve_volume=getattr(x,'use_deform_preserve_volume',None),vertex_groups=getattr(x,'use_vertex_groups',None),envelopes=getattr(x,'use_bone_envelopes',None)) for x in o.modifiers],material_slots=[s.material.name if s.material else None for s in o.material_slots])
def validate(p):
 assert p['schema']=='hone.source-preservation.v1'
 assert p['runtime_ready'] is False
 for m in p['meshes']:
  n=len(m['vertices']); assert len(m['weights'])==n
  for poly in m['polygons']:
   assert isinstance(poly['smooth'],bool)
   assert all(0<=i<n for i in poly['vertices'])
  for t in m['triangles']:
   assert len(t['vertices'])==3 and all(0<=i<n for i in t['vertices'])
   assert 0<=t['polygon']<len(m['polygons'])
   assert all(0<=i<len(m['corner_normals']) for i in t['loops'])
  if m['shape_keys']:
   names={k['name'] for k in m['shape_keys']['blocks']}
   for k in m['shape_keys']['blocks']:
    assert len(k['positions'])==n and k['relative_key'] in names
  for w in m['weights']:
   assert all(0<=i<len(m['vertex_groups']) and math.isfinite(v) for i,v in w)
 return True

def export(src,dest,exclude):
 src=pathlib.Path(src);dest=pathlib.Path(dest)
 digest=hashlib.sha256(src.read_bytes()).hexdigest()
 bpy.ops.wm.open_mainfile(filepath=str(src.resolve()),use_scripts=False)
 objects=[o for o in bpy.context.scene.objects if o.type=='MESH']
 assert set(exclude)<=set(o.name for o in objects), 'Unknown explicit exclusion'
 meshes=[mesh_record(o) for o in objects if o.name not in exclude]
 rigs=[dict(name=o.name,matrix_world=matrix(o.matrix_world),bones=[dict(name=b.name,parent=b.parent.name if b.parent else None,matrix_local=matrix(b.matrix_local),deform=b.use_deform,inherit_scale=b.inherit_scale) for b in o.data.bones],pose=[dict(name=b.name,matrix_basis=matrix(b.matrix_basis),matrix=matrix(b.matrix),constraints=[c.type for c in b.constraints]) for b in o.pose.bones]) for o in bpy.context.scene.objects if o.type=='ARMATURE']
 p=dict(schema='hone.source-preservation.v1',runtime_ready=False,source_sha256=digest,blender=bpy.app.version_string,excluded_objects=exclude,meshes=meshes,armatures=rigs,unresolved=['material node graph and opacity baking','animation actions/drivers and constraint evaluation','runtime deformation and corner-normal adapter','same-head private broker build and hardware validation'])
 validate(p);dest.write_text(json.dumps(p,separators=(',',':'),allow_nan=False))
 q=json.loads(dest.read_text());validate(q);assert q==p
 # Exact JSON round-trip of every mesh means all shape coordinates and mixed normals survived serialization.
 assert hashlib.sha256(src.read_bytes()).hexdigest()==digest,'Source mutated'
 return dict(source_sha256=digest,package_sha256=hashlib.sha256(dest.read_bytes()).hexdigest(),meshes=len(meshes),triangles=sum(len(m['triangles']) for m in meshes),shape_key_meshes=sum(m['shape_keys'] is not None for m in meshes),flat_polygons=sum(not f['smooth'] for m in meshes for f in m['polygons']),roundtrip='exact',runtime_ready=False)
if __name__=='__main__':
 args=sys.argv[sys.argv.index('--')+1:]
 print(json.dumps(export(args[0],args[1],args[2:]),indent=2))
