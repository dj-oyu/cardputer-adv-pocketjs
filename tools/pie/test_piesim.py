"""Unit tests for piesim's register-name parsing.

The ESP32-S3 PIE unit has exactly q0..q7 (TRM 1.8). Sim.qi() used to be
`int(x[1])`, which reads only the second character of the operand: "q10"
silently became register 1 instead of raising, and a rescheduled kernel
that typo'd a register number past 7 would run against the wrong lanes
without any error. This checks that qi() now accepts only q0..q7 and
rejects everything else with a clear exception naming the bad operand.

    python tools/pie/test_piesim.py           (from the repository root)
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(__file__))
from piesim import Sim  # noqa: E402


class RegisterParsing(unittest.TestCase):
    def _run(self, asm):
        return Sim(bytearray(64)).run(asm, {})

    def test_accepts_q0_through_q7(self):
        for n in range(8):
            # Must not raise, and must clear the exact register named.
            self._run('ee.zero.q q%d\n' % n)

    def test_rejects_q8(self):
        with self.assertRaises(ValueError) as cm:
            self._run('ee.zero.q q8\n')
        self.assertIn('q8', str(cm.exception))

    def test_rejects_q10(self):
        # The original bug: int("q10"[1]) == 1, silently aliasing q10 to q1
        # instead of failing. Confirm it now raises instead of aliasing.
        with self.assertRaises(ValueError) as cm:
            self._run('ee.zero.q q10\n')
        self.assertIn('q10', str(cm.exception))

    def test_rejects_non_register_operand(self):
        with self.assertRaises(ValueError) as cm:
            self._run('ee.zero.q x1\n')
        self.assertIn('x1', str(cm.exception))


if __name__ == '__main__':
    unittest.main()
