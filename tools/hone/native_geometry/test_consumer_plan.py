import copy,unittest
from consumer_plan import prepare,admit_runtime,Unsupported

def fixture():
    pts=[[0,0,0],[1,0,0],[0,1,0],[0,0,1]]
    key=dict(name='Basis',relative_key='Basis',positions=pts)
    return dict(schema='hone.source-preservation.v1',runtime_ready=False,meshes=[dict(name='mixed',vertices=pts,corner_normals=[[0,0,1]]*3+[[0,1,0]]*3,weights=[[] for _ in pts],vertex_groups=[],shape_keys=dict(use_relative=True,blocks=[key,dict(key,name='Smile',positions=[*pts[:3],[0,0,.5]])]),polygons=[dict(vertices=[0,1,2],loops=[0,1,2],smooth=True),dict(vertices=[0,3,1],loops=[3,4,5],smooth=False)],triangles=[dict(vertices=[0,1,2],loops=[0,1,2],polygon=0),dict(vertices=[0,3,1],loops=[3,4,5],polygon=1)])])
class Test(unittest.TestCase):
    def test_split_and_morph(self):
        p=prepare(fixture());m=p['meshes'][0]
        self.assertEqual(m['source_vertices'],[0,1,2,0,3,1]);self.assertEqual(m['polygon_smooth'],[True,False])
        self.assertEqual(m['shape_keys']['blocks'][1]['positions'][4],[0,0,.5])
        self.assertNotEqual(m['rest_corner_normals'][0],m['rest_corner_normals'][3])
        with self.assertRaises(Unsupported):admit_runtime(p)
    def test_reject_corrupt(self):
        for change in [lambda m:m['triangles'][0]['loops'].__setitem__(0,3),lambda m:m['vertices'][0].__setitem__(0,float('nan')),lambda m:m['shape_keys']['blocks'][0]['positions'].pop()]:
            p=copy.deepcopy(fixture());change(p['meshes'][0])
            with self.assertRaises(ValueError):prepare(p)
if __name__=='__main__':unittest.main()
