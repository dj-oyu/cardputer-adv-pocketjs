import copy, unittest
from frame_memory_plan import inspect
from test_provider_binding import source

class TestFrameMemoryPlan(unittest.TestCase):
    def test_sequential_workspace_and_node_budget(self):
        p=source();p['meshes'].append(copy.deepcopy(p['meshes'][0]))
        r=inspect(p,252);b=r['bytes']
        self.assertEqual(r['counts']['total_declared_nodes'],256)
        self.assertFalse(r['declared_nodes_exceed_limit'])
        self.assertEqual(b['pending_source_positions'],96)
        self.assertEqual(b['pending_packed_normals'],144)
        self.assertEqual(b['normal_workspace_per_mesh_total'],192)
        self.assertEqual(b['normal_workspace_sequential'],96)
        self.assertEqual(b['normal_workspace_saved'],96)
        self.assertEqual(b['pending_geometry_plus_shared_normal_work'],336)
        self.assertEqual(b['dense_morph_masks'],64)
        self.assertTrue(inspect(p,253)['declared_nodes_exceed_limit'])
        self.assertFalse(r['runtime_ready'])
    def test_empty_and_bad_input(self):
        p=source();p['meshes']=[]
        self.assertEqual(inspect(p)['bytes']['normal_workspace_sequential'],0)
        with self.assertRaises(ValueError):inspect(p,True)
        with self.assertRaises(ValueError):inspect(p,-1)
        p=source();p['meshes'][0]['polygons'][0]['material']=999
        with self.assertRaises(ValueError):inspect(p)

if __name__=='__main__':unittest.main()
