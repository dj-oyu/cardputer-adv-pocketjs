import unittest,struct,copy
from test_consumer_plan import fixture
from consumer_plan import prepare
from source_snapshot import topology_identity,map_snapshot
class Test(unittest.TestCase):
 def test_preserve(self):
  p=fixture();m=p['meshes'][0];plan=prepare(p)['meshes'][0];s=dict(frame=7,source_sha256='synthetic',topology_sha256=topology_identity(m),positions=m['vertices'],corner_normals=m['corner_normals'])
  r=map_snapshot(m,plan,s,7,'synthetic');self.assertEqual(len(r['bytes']),6*24);self.assertFalse(r['runtime_ready'])
  self.assertEqual(struct.unpack_from('<6f',r['bytes'],3*24)[3:],(0.,1.,0.))
  with self.assertRaises(ValueError):map_snapshot(m,plan,s,8,'synthetic')
  s['corner_normals']=copy.deepcopy(s['corner_normals']);s['corner_normals'][0]=[0,0,0]
  with self.assertRaises(ValueError):map_snapshot(m,plan,s,7,'synthetic')
if __name__=='__main__':unittest.main()
