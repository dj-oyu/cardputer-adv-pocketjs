import copy
import unittest
from unittest.mock import patch
from scene_node_plan import plan
from provider_node_map import emit
from test_provider_binding import source


class TestSceneNodePlan(unittest.TestCase):
    def fixture(self):
        package = source()
        package['source_sha256'] = 'synthetic'
        pose = [1, 2, 3, 0, 0, 0, 1, 1, 1]
        hierarchy = dict(schema='hone.scene-hierarchy-snapshot.v1', runtime_ready=False,
            source_sha256='synthetic', frame=1, armature_bones=2, controls=[{'data_path':'value'}],
            parents=[dict(name='parent',parent=None,pose=pose,hide_render=False)],
            meshes=[dict(name='mixed',parent='parent',pose=pose,hide_render=True)])
        return package, hierarchy

    def test_shared_parent_and_provider_mapping(self):
        package, hierarchy = self.fixture()
        result = plan(package, hierarchy)
        self.assertFalse(result['runtime_ready'])
        self.assertEqual([n['parent'] for n in result['nodes']], [-1, 0, 0])
        self.assertEqual(result['provider_assignments'], [None, 0, 1])
        self.assertEqual([n['enabled'] for n in result['nodes']], [True, False, False])
        self.assertIn('hone_provider_node_count = 3', emit(package, result['provider_assignments']))

    def test_identity_cycle_and_invalid_pose_reject(self):
        package, original = self.fixture()
        cases = []
        h=copy.deepcopy(original);h['source_sha256']='wrong';cases.append(h)
        h=copy.deepcopy(original);h['parents'][0]['parent']='parent';cases.append(h)
        h=copy.deepcopy(original);h['meshes'][0]['pose'][6]=0;cases.append(h)
        h=copy.deepcopy(original);h['meshes'][0]['name']='other';cases.append(h)
        for h in cases:
            with self.subTest(h=h), self.assertRaises(ValueError):plan(package,h)

    def test_hidden_transform_ancestor_keeps_visible_mesh(self):
        package, hierarchy = self.fixture()
        hierarchy['parents'][0]['hide_render'] = True
        hierarchy['meshes'][0]['hide_render'] = False
        result = plan(package, hierarchy)
        self.assertEqual([n['parent'] for n in result['nodes']], [-1, 0, 0])
        self.assertEqual([n['enabled'] for n in result['nodes']], [True, True, True])

    def test_total_nodes_not_just_geometry_must_fit(self):
        package, hierarchy = self.fixture()
        with patch('scene_node_plan.binding_plan',return_value={'nodes':[{'source_mesh':0}]*256}):
            with self.assertRaisesRegex(ValueError,'node limit'):plan(package,hierarchy)


if __name__ == '__main__':
    unittest.main()
