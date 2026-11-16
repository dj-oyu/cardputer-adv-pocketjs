import unittest
from provider_partition import partition_mesh, plan
from consumer_plan import prepare
from test_consumer_plan import fixture

class TestPartition(unittest.TestCase):
    def test_order_and_exact_reconstruction(self):
        source = [list(range(i, i+3)) for i in range(0,99,3)]
        mesh = dict(positions=[[0,0,0]]*99, triangles=source)
        parts = partition_mesh(mesh)
        self.assertEqual(len(parts),4)
        self.assertEqual([i for p in parts for i in p['source_triangles']],list(range(33)))
        rebuilt = [[p['prepared_vertices'][v] for v in t] for p in parts for t in p['triangles']]
        self.assertEqual(rebuilt,source)
        self.assertTrue(all(len(p['prepared_vertices'])<=32 for p in parts))
    def test_shared_corner_and_empty(self):
        mesh = dict(positions=[[0,0,0]]*4, triangles=[[0,1,2],[0,2,3]])
        self.assertEqual(len(partition_mesh(mesh)),1)
        self.assertEqual(len(partition_mesh(mesh,3)),2)
        self.assertEqual(partition_mesh(dict(positions=[],triangles=[])),[])
    def test_triangle_cap_and_attribute_gather(self):
        many = dict(positions=[[0,0,0]]*3,triangles=[[0,1,2]]*129)
        self.assertEqual([len(p['triangles']) for p in partition_mesh(many)],[64,64,1])
        package=fixture(); raw=package['meshes'][0]
        raw['vertex_groups']=['joint']; raw['weights'][3]=[[0,0.75]]
        raw['triangles'].reverse()
        mesh=prepare(package)['meshes'][0]
        parts=partition_mesh(mesh,3)
        self.assertEqual(len(parts),2)
        for part in parts:
            for local,prepared in enumerate(part['prepared_vertices']):
                original=mesh['source_vertices'][prepared]
                corner=mesh['source_corners'][prepared]
                self.assertEqual(mesh['positions'][prepared],raw['vertices'][original])
                self.assertEqual(mesh['weights'][prepared],raw['weights'][original])
                self.assertEqual(mesh['rest_corner_normals'][prepared],raw['corner_normals'][corner])
                for k,block in enumerate(mesh['shape_keys']['blocks']):
                    self.assertEqual(block['positions'][prepared],raw['shape_keys']['blocks'][k]['positions'][original])
    def test_invalid(self):
        for row in [[0,1,3],[0,1,-1],[0,1,True],[0,1]]:
            with self.assertRaises(ValueError): partition_mesh(dict(positions=[[0,0,0]]*3,triangles=[row]))
        for limit in [2,33,True,3.0]:
            with self.assertRaises(ValueError):partition_mesh(dict(positions=[],triangles=[]),limit)
    def test_never_runtime_admit(self):
        result = plan(fixture())
        self.assertFalse(result['runtime_ready'])
        self.assertEqual(result['geometry_nodes'],1)

if __name__ == '__main__': unittest.main()
