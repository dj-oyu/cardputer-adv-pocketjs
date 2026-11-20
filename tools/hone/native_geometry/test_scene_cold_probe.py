import copy
import unittest
from scene_cold_probe import build
from scene_node_plan import plan
from test_scene_node_plan import TestSceneNodePlan


class TestSceneColdProbe(unittest.TestCase):
    def test_probe_retains_degenerate_geometry_and_marks_materials(self):
        package,hierarchy=TestSceneNodePlan().fixture()
        package['meshes'][0]['vertices'][2]=list(package['meshes'][0]['vertices'][1])
        scene=plan(package,hierarchy);probe=build(package,scene)
        self.assertFalse(probe['runtime_ready'])
        self.assertIn('PLACEHOLDER MATERIALS',probe['warnings'][0])
        self.assertEqual(len(probe['payload']['nodes']),3)
        mesh=probe['payload']['nodes'][1]['mesh']
        self.assertEqual(mesh['vertices'][3:6],mesh['vertices'][6:9])
        self.assertEqual(mesh['indices'],[0,1,2])

    def test_duplicate_or_wrong_source_rejects(self):
        package,hierarchy=TestSceneNodePlan().fixture();scene=plan(package,hierarchy)
        duplicate=copy.deepcopy(scene);duplicate['nodes'][2]['part']=0
        with self.assertRaises(ValueError):build(package,duplicate)
        wrong=copy.deepcopy(scene);wrong['source_sha256']='other'
        with self.assertRaises(ValueError):build(package,wrong)


if __name__=='__main__':unittest.main()
