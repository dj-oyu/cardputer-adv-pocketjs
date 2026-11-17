import unittest
from provider_node_map import emit
from test_provider_binding import source


class TestProviderNodeMap(unittest.TestCase):
    def test_nonmesh_holes_and_explicit_reordering(self):
        text = emit(source(), [None, 1, None, 0])
        self.assertIn('hone_provider_node_1_vertices', text)
        self.assertIn('hone_provider_node_3_vertices', text)
        self.assertIn('hone_provider_node_count = 4', text)
        self.assertEqual(text.count('{UINT32_MAX,0,0}'), 2)

    def test_missing_duplicate_and_invalid_assignments_reject(self):
        for assignment in ([0], [0, 0], [0, 1, 2], [False, 1], [0, -1], [None] * 257):
            with self.subTest(assignment=assignment), self.assertRaises(ValueError):
                emit(source(), assignment)


if __name__ == '__main__':
    unittest.main()
