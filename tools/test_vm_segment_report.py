import copy
import unittest
from vm_l0_capture import SEGMENT_SIZES
from vm_segment_report import summarize, merge_repairs


def fixture():
    rows = []
    for policy, (first, maximum) in enumerate(SEGMENT_SIZES):
        for app in 'XABCDEF':
            for rep in range(2):
                for seq in range(2):
                    rows.append(dict(letter=app, rep=rep, seq=seq, heap_free_min=100,
                        frames=1, samples={m: [1] for m in ('frame', 'call', 'drain', 'jobs')},
                        heap_largest_min=80, ended_early=False,
                        segment_apply=dict(policy=policy, first=first, max=maximum, result=0),
                        segment=dict(seg_first=first, seg_max=maximum, held_max=50, live_max=40,
                                     frame_max=20, depth_max=2, seg_mallocs=1, pushes=4)))
    return rows


class SegmentReportTest(unittest.TestCase):
    def test_complete_matrix(self):
        result = summarize(fixture())
        self.assertEqual(len(result), 84)
        self.assertEqual(result[0]['largest'], 80)

    def test_missing_run(self):
        with self.assertRaisesRegex(ValueError, 'matrix mismatch'):
            summarize(fixture()[2:])

    def test_duplicate_window(self):
        rows = fixture()
        rows.append(copy.deepcopy(rows[0]))
        with self.assertRaisesRegex(ValueError, 'duplicate'):
            summarize(rows)

    def test_unverified_or_lost_data(self):
        for field, value in [('ended_early', True), ('drainrun_drop', 1), ('depth_drop', 1)]:
            rows = fixture()
            rows[0][field] = value
            with self.assertRaises(ValueError):
                summarize(rows)
        rows = fixture()
        rows[0]['segment_apply']['result'] = -1
        with self.assertRaisesRegex(ValueError, 'unverified'):
            summarize(rows)
        rows = fixture()
        rows[0]['segment']['seg_first'] += 16
        with self.assertRaisesRegex(ValueError, 'sizes differ'):
            summarize(rows)

    def test_missing_samples(self):
        rows = fixture()
        rows[0]['samples']['frame'] = []
        with self.assertRaisesRegex(ValueError, 'missing frame samples'):
            summarize(rows)

    def test_repair_replaces_whole_failed_run(self):
        rows = fixture()
        replacement = copy.deepcopy(rows[:2])
        rows[0]['samples'] = {}
        repaired = merge_repairs(rows, replacement)
        self.assertEqual(len(summarize(repaired)), 84)
        self.assertEqual(rows[0]['samples'], {})  # Original evidence is untouched.
        with self.assertRaisesRegex(ValueError, 'existing incomplete'):
            merge_repairs(fixture(), replacement)
        replacement[0]['samples'] = {}
        with self.assertRaisesRegex(ValueError, 'still lacks'):
            merge_repairs(rows, replacement)


if __name__ == '__main__':
    unittest.main()
