import unittest
from scene_preflight import inspect
from test_provider_binding import source


class TestScenePreflight(unittest.TestCase):
    def fixture(self):
        p = source()
        p['meshes'][0].update(has_custom_normals=False, sharp_edges=[], modifiers=[])
        return p

    def test_geometry_budget_and_modifier_disagreement(self):
        p = self.fixture()
        p['meshes'][0]['modifiers'] = [dict(type='ARMATURE', viewport=True, render=False)]
        result = inspect(p)
        self.assertFalse(result['runtime_ready'])
        self.assertEqual(result['geometry_nodes'], 2)
        self.assertEqual(result['remaining_slots_before_other_nodes'], 254)
        self.assertEqual(result['counts']['viewport_render_modifier_differences'], 1)
        self.assertEqual(result['viewport_modifiers'], {'ARMATURE': 1})
        self.assertTrue(result['cold_nonzero_area_check_passes'])

    def test_degenerate_triangle_is_reported_without_removal(self):
        p = self.fixture()
        p['meshes'][0]['vertices'][2] = list(p['meshes'][0]['vertices'][1])
        result = inspect(p)
        self.assertEqual(result['counts']['triangles'], 2)
        self.assertEqual(result['counts']['cold_zero_area_triangles'], 1)
        self.assertFalse(result['cold_nonzero_area_check_passes'])

    def test_missing_normal_metadata_fails_closed(self):
        p = self.fixture()
        del p['meshes'][0]['sharp_edges']
        with self.assertRaises(ValueError):
            inspect(p)


if __name__ == '__main__':
    unittest.main()
