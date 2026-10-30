"""Capture planning and failed-run provenance; no compiler or device required."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import capture_character as capture


class CapturePlanTests(unittest.TestCase):
    def test_large_range_rejected_before_iteration(self):
        class UniterableRange:
            def __len__(self):
                return 1000000001

            def __iter__(self):
                raise AssertionError('oversized range was enumerated')

        with patch.object(capture, 'range', return_value=UniterableRange(), create=True):
            with self.assertRaisesRegex(ValueError, 'at most 1000'):
                capture.ticks('0:1000000000')
        with self.assertRaisesRegex(ValueError, 'endpoints'):
            capture.ticks('0:1000000001')

    def test_reverse_ranges_preserve_requested_order_and_duplicates(self):
        self.assertEqual(capture.ticks('92:80:-4,82,82,0:2'),
                         [92, 88, 84, 80, 82, 82, 0, 1, 2])
        with self.assertRaisesRegex(ValueError, 'step'):
            capture.ticks('80:92:-1')

    def test_prepare_removes_only_planned_images_on_rerun(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            _, records = capture.prepare(folder)
            self.assertEqual(len(records), 12)
            for record in records:
                for kind in ('ppm', 'png'):
                    (folder / record[kind]).write_bytes(b'previous run')
            unrelated = folder / 'unrelated.png'
            unrelated.write_bytes(b'keep this file')
            plan, rerun = capture.prepare(folder)
            self.assertEqual(records, rerun)
            self.assertEqual(unrelated.read_bytes(), b'keep this file')
            for record in records:
                self.assertFalse((folder / record['ppm']).exists())
                self.assertFalse((folder / record['png']).exists())
            self.assertEqual(len(plan.read_text().splitlines()), 12)

    def test_failed_run_keeps_missing_captures_and_failure_in_manifest(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            _, records = capture.prepare(folder)
            manifest_path = capture.finish(folder, records, {'head': 'test-only'},
                                           False, 'fixture failed before first capture\n')
            manifest = json.loads(manifest_path.read_text())
            self.assertFalse(manifest['native_checks_passed'])
            self.assertEqual(manifest['kind'], 'HOST SIMULATION')
            self.assertFalse(manifest['device_capture'])
            self.assertEqual(len(manifest['captures']), 12)
            for record in manifest['captures']:
                self.assertTrue(record['missing'])
                self.assertNotIn('rgb_sha256', record)
                self.assertNotIn('png_sha256', record)
            self.assertEqual(list(folder.glob('*.png')), [])
            self.assertIn('fixture failed', (folder / manifest['log']).read_text())


if __name__ == '__main__':
    unittest.main()
