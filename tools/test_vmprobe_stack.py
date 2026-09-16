"""The stack probe's lifetime HWM and instantaneous samples must stay distinct."""
import unittest
from vm_l0_capture import Collector, STATIC_RE
from vm_stack_report import check_curve


class StackProbeTest(unittest.TestCase):
    def test_depth_control(self):
        self.assertEqual(check_curve([1, 2, 4], [10000] * 3, 'flat')['span_bytes'], 0)
        self.assertEqual(check_curve([1, 2, 4], [10000, 9900, 9700], 'recur')['bytes_per_depth_min'], 100)
        with self.assertRaises(ValueError):
            check_curve([1, 2, 4], [10000] * 3, 'recur')
        with self.assertRaises(ValueError):
            check_curve([1, 2, 4], [10000, 9900, 9700], 'flat')
        with self.assertRaises(ValueError):
            check_curve([1, 2, 4], [], 'flat')

    def test_scope_backwards_compatible(self):
        line = ('VMPROBE STATIC engine=test compiler=gcc opt=Os sizeof_jsvalue=8 '
                'sizeof_stackframe=48 sizeof_varref=24 fw=test cond=0')
        self.assertIsNone(STATIC_RE.search(line).group('stack_scope'))
        match = STATIC_RE.search(line + ' stack_scope=task_lifetime stack_unit=bytes')
        self.assertEqual(match.group('stack_scope'), 'task_lifetime')
        self.assertEqual(match.group('stack_unit'), 'bytes')

    def test_instantaneous_depth_not_historical_minimum(self):
        c = Collector()
        c.feed('VMPROBE WINDOW seq=0 cond=0 ms=1000 frames=1 lat_n=0 lat_drop=0 '
               'qpeak_max=0 heap_free_min=100 heap_largest_min=80 js_used_max=20 '
               'js_limit=100 stack_hw_min=500 flush_us=1 drainrun_drop=0 depth_drop=0')
        c.feed('VMPROBE S 0 depth 3 1,2,4')
        c.feed('VMPROBE S 0 depth_hwm 3 500,500,500')
        c.feed('VMPROBE S 0 depth_fp 3 10000,9900,9700')
        c.close()
        sample = c.records[0]['samples']
        self.assertEqual(sample['depth_hwm'], [500, 500, 500])
        self.assertEqual(sample['depth_fp'][0] - sample['depth_fp'][-1], 300)
        self.assertEqual(c.bad, 0)
        self.assertEqual(c.records[0]['depth_drop'], 0)


if __name__ == '__main__':
    unittest.main()
