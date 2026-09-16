"""D42 capture must retain the final segment report printed during teardown."""
import unittest
import sys
import tempfile
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch
from vm_l0_capture import Collector, run_one, main


class SerialFixture:
    def __init__(self):
        self.now = 0.0
        self.running = False
        self.lines = []
        self.closed = False

    def close(self):
        self.closed = True

    def reset_input_buffer(self):
        self.lines.clear()

    def write(self, data):
        if data == b'q':
            if self.running:
                self.lines.append(b'#info vmstack seg_first=512 seg_max=4096 held_max=547\n')
            self.lines.append(b'HOME_READY\n')
        elif data == b'J':
            self.lines.append(b'VMSEG SELECT policy=3\n')
        elif data == b'F':
            self.running = True
            self.lines.extend([
                b'VMSEG APPLY policy=3 first=512 max=4096 result=0\n',
                b'VMPROBE WINDOW seq=0 cond=0 ms=1000 frames=0 lat_n=0 lat_drop=0 '
                b'qpeak_max=0 heap_free_min=100 heap_largest_min=80 js_used_max=20 '
                b'js_limit=100 stack_hw_min=500 flush_us=1\n',
            ])

    def readline(self):
        self.now += 0.1
        return self.lines.pop(0) if self.lines else b''


class SegmentCaptureTest(unittest.TestCase):
    def test_window_split_by_capture_deadline(self):
        class SplitSerial(SerialFixture):
            def write(self, data):
                super().write(data)
                if data == b'F':
                    self.lines[-1] = self.lines[-1].replace(b'frames=0', b'frames=1')
                    self.lines.extend([f'VMPROBE S 0 {m} 1 42\n'.encode()
                                       for m in ('frame', 'call', 'drain', 'jobs')])

            def readline(self):
                line = super().readline()
                if b'VMPROBE WINDOW' in line:
                    self.now += 2  # Stop reading just after the header.
                return line

        ser = SplitSerial()
        with patch('vm_l0_capture.time.monotonic', side_effect=lambda: ser.now), \
             patch('vm_l0_capture.time.sleep'):
            collector, early, _ = run_one(ser, 'F', 'base', 1, False, 3)
        self.assertFalse(early)
        self.assertEqual(collector.records[0]['frames'], 1)
        self.assertEqual(collector.records[0]['samples']['frame'], [42])
        self.assertEqual(collector.records[0]['samples']['jobs'], [42])

    def test_teardown_report(self):
        ser = SerialFixture()
        with patch('vm_l0_capture.time.monotonic', side_effect=lambda: ser.now), \
             patch('vm_l0_capture.time.sleep'):
            collector, early, returned = run_one(ser, 'F', 'base', 1, False, 3)
        self.assertIs(returned, ser)
        self.assertFalse(early)
        self.assertEqual(collector.segment_apply['policy'], 3)
        self.assertEqual(collector.segment_apply['result'], 0)
        self.assertEqual(collector.segment['held_max'], 547)
        self.assertEqual(len(collector.records), 1)

    def test_rejected_apply_is_not_success(self):
        c = Collector()
        c.feed('VMSEG APPLY policy=3 first=512 max=4096 result=-1')
        self.assertEqual(c.segment_apply['result'], -1)
        self.assertIsNone(c.segment)

    def test_failed_capture_exits_nonzero(self):
        ser = SerialFixture()
        with tempfile.TemporaryDirectory() as directory, \
             patch.dict(sys.modules, {'serial': SimpleNamespace(Serial=lambda *a, **k: ser)}), \
             patch('vm_l0_capture.time.sleep'), \
             patch('vm_l0_capture.run_one', side_effect=RuntimeError('injected missing response')), \
             patch.object(sys, 'argv', ['capture', '--port', 'FAKE', '--workloads', 'F',
                 '--conditions', 'base', '--out', str(Path(directory) / 'failed.jsonl')]):
            with self.assertRaises(SystemExit) as raised:
                main()
        self.assertEqual(raised.exception.code, 1)
        self.assertTrue(ser.closed)


if __name__ == '__main__':
    unittest.main()
