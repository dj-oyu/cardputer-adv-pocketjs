import unittest,copy
from test_consumer_plan import fixture
from consumer_plan import prepare
from cold_geometry import partition,deformed_corner_normals
class Test(unittest.TestCase):
 def test_normal_after_deformation(self):
  p=fixture();m=p['meshes'][0]
  n=deformed_corner_normals(m,m['vertices']);self.assertEqual(n[0],[0.,0.,1.]);self.assertEqual(n[3],[0.,1.,0.])
  v=copy.deepcopy(m['vertices']);v[2][2]=1
  after=deformed_corner_normals(m,v);self.assertNotEqual(after[0],n[0]);self.assertEqual(after[3],n[3])
 def test_cold_identity(self):
  p=fixture();m=p['meshes'][0]
  for f in m['polygons']:f['material']=0
  plan=prepare(p)['meshes'][0];r=partition(m,plan);self.assertEqual(len(r['chunks']),1)
  c=r['chunks'][0];self.assertEqual([[c['source_plan_vertices'][i] for i in t] for t in c['triangles']],plan['triangles'])
  m['polygons'][1]['material']=1;r2=partition(m,plan);self.assertEqual(len(r2['chunks']),2);self.assertNotEqual(r['topology_sha256'],r2['topology_sha256'])
 def test_unsupported_normals(self):
  m=fixture()['meshes'][0];m['has_custom_normals']=True
  with self.assertRaises(ValueError):deformed_corner_normals(m,m['vertices'])
if __name__=='__main__':unittest.main()
