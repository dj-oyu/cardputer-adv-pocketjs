import copy,hashlib,unittest
from export_material_payload_fixture import export
from test_consumer_plan import fixture
import test_alpha_factor_compile as alpha_fixture

class ExportTest(unittest.TestCase):
 def inputs(self):
  source=fixture();m=source['meshes'][0];m['material_slots']=['mat']
  for p in m['polygons']:p['material']=0
  inv,_=alpha_fixture.TestAlphaFactor().fixture();mat=inv['materials'][0];mat.update(library=None,use_nodes=True,uses=[dict(object=m['name'],slot=0,polygons=[0,1])])
  next(n for n in mat['nodes'] if n['type']=='ShaderNodeBsdfPrincipled')['inputs'].append(alpha_fixture.sock('Base Color',1,[.2,.3,.4,1]))
  drivers=dict(schema='hone.driver-route-plan.v1',runtime_ready=False,routes=[dict(kind='shader_property',program=0,owner=dict(embedded_in=[dict(type='Material',name='mat')]),data_path='nodes["value"].outputs[0].default_value',array_index=0)])
  scene=dict(nodes=[dict(index=1,kind='gouraudMesh',source_mesh=0,part=0)])
  for x in (source,inv,drivers,scene):x['source_sha256']=hashlib.sha256(b'original').hexdigest()
  return source,b'original',inv,drivers,scene
 def test_emit_and_bytes_identity(self):
  args=self.inputs();result=export(*args);self.assertTrue(result[3]['source_bytes_verified']);self.assertEqual(result[3]['polygons'],2)
  with self.assertRaises(ValueError):export(args[0],b'changed',*args[2:])
 def test_route_mismatch(self):
  args=self.inputs();args[2]['materials'][0]['uses'][0]['polygons']=[0]
  with self.assertRaises(ValueError):export(*args)
  args=self.inputs();args[4]['nodes']*=2
  with self.assertRaises(ValueError):export(*args)
if __name__=='__main__':unittest.main()
