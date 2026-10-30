"""Numerical/replay tests only; execution time is not a benchmark."""

import math
import unittest
from unittest.mock import patch

from signals import I16_SCALE, U32_MASK, U32_RANGE, LutDrive, make_lut


class SignalTests(unittest.TestCase):
    def test_table_and_payload(self):
        for n in (32, 64, 256):
            table = make_lut(n)
            self.assertIsInstance(table, tuple)
            self.assertEqual(len(table), n)
            self.assertEqual(table[0], 0)
            self.assertEqual(table[n // 4], I16_SCALE)
            self.assertEqual(table[3 * n // 4], -I16_SCALE)
            self.assertTrue(all(type(x) is int and -32768 <= x <= 32767 for x in table))
            print(f"SIGNAL_PAYLOAD n={n} table={n*2} phase_inc_weight=36 core={n*2+36} "
                  f"reset_seed=4 with_reset={n*2+40} bytes excluding_python_overhead=1")

    def test_same_phase_interpolation_error(self):
        # Exactly the same phase quantization is used in both paths. These
        # errors exclude frequency quantization and compare weighted outputs.
        previous_max = math.inf
        for n in (32, 64, 256):
            drive, largest, squares, count = LutDrive(n), 0., 0., 36000
            for _ in range(count):
                expected = sum(w * math.sin(math.tau * phase / U32_RANGE)
                               for w, phase in zip(drive.weights, drive.phases))
                error = abs(drive.step() - expected)
                largest = max(largest, error)
                squares += error * error
            # Linear sine interpolation <= h²/8, plus int16 rounding error.
            bound = (math.tau / n) ** 2 / 8 + .5 / I16_SCALE
            self.assertLessEqual(largest, bound)
            self.assertLess(largest, previous_max)
            previous_max = largest
            print(f"SIGNAL_ERROR n={n} samples={count} max={largest:.12g} "
                  f"rms={math.sqrt(squares/count):.12g} analytic_bound={bound:.12g}")

    def test_bound_reset_pause_seed(self):
        drive = LutDrive()
        start = drive.snapshot()
        expected = [drive.step() for _ in range(12000)]
        self.assertTrue(all(-1 <= value <= 1 and math.isfinite(value) for value in expected))
        frozen = drive.snapshot()
        for _ in range(100):
            self.assertEqual(drive.snapshot(), frozen)  # Paused: no step.
        drive.reset()
        self.assertEqual(drive.snapshot(), start)
        self.assertEqual([drive.step() for _ in expected], expected)
        other = LutDrive(seed=43)
        self.assertNotEqual(other.snapshot()[0], start[0])
        self.assertEqual(other.increments, drive.increments)
        self.assertEqual(other.weights, drive.weights)

    def test_wrap_seam_and_no_per_step_sin(self):
        drive = LutDrive()
        drive.phases[:] = [U32_MASK] * 3
        before = tuple(drive.phases)
        with patch('signals.math.sin', side_effect=AssertionError('per-step sin')):
            value = drive.step()
        self.assertAlmostEqual(value, 0, delta=math.tau / U32_RANGE + .5 / I16_SCALE)
        self.assertEqual(drive.phases,
                         [(p + inc) & U32_MASK for p, inc in zip(before, drive.increments)])
        self.assertTrue(all(0 <= p <= U32_MASK for p in drive.phases))
        self.assertEqual(LutDrive(seed=0).snapshot(), LutDrive(seed=0).snapshot())

    def test_invalid_setup(self):
        for n in (True, 1, 65537, 64.5):
            with self.assertRaises(ValueError):
                make_lut(n)
        for seed in (True, -1, U32_RANGE, 1.5):
            with self.assertRaises(ValueError):
                LutDrive(seed=seed)
        for hz in (True, 0, -1, 2.26, math.inf, math.nan):
            with self.assertRaises(ValueError):
                LutDrive(step_hz=hz)


if __name__ == '__main__':
    unittest.main(verbosity=2)
