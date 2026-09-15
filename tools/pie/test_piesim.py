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
from piesim import Sim, load16, store16  # noqa: E402


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


class FirInstructions(unittest.TestCase):
    """The instructions the MP3 FIR kernel needs, against their TRM pseudocode.

    1.8.90 `ee.ld.128.usar.ip` + 1.8.52 `ee.src.q` are the unaligned-window pair
    (docs/perf/pie-simd.md 1.4): the load brings in the aligned sixteen bytes and
    leaves the byte offset in SAR_BYTE, and the pair shifts the concatenation
    right by that many bytes -- with the *first* operand as the low half. If
    either half is swapped the window reads from the wrong side of the address
    and every tap of the kernel is off by one sample, silently.
    """

    RAMP = 0x100

    def _ramp_mem(self):
        mem = bytearray(0x400)
        for i in range(48):
            v = (i * 37 + 11) & 0xFFFF
            mem[self.RAMP + 2 * i] = v & 0xFF
            mem[self.RAMP + 2 * i + 1] = v >> 8
        return mem

    def test_usar_src_q_reads_the_unaligned_window(self):
        asm = 'ee.ld.128.usar.ip q0, a3, 16\nee.ld.128.usar.ip q1, a3, 16\nee.src.q q2, q0, q1\n'
        for off in range(8):
            mem = self._ramp_mem()
            sim = Sim(mem)
            sim.run(asm, {'a3': self.RAMP + 2 * off})
            got = sim.q[2]
            want = [(mem[self.RAMP + 2 * off + 2 * i] | (mem[self.RAMP + 2 * off + 2 * i + 1] << 8))
                    for i in range(8)]
            self.assertEqual(got, want, f'byte offset {2 * off}')
            self.assertEqual(sim.sar_byte, (2 * off) & 15, 'SAR_BYTE')
            self.assertEqual(sim.ar['a3'], self.RAMP + 2 * off + 32, 'the pointer advanced by two loads')

    def test_usar_src_q_swapped_halves_read_the_wrong_side(self):
        # The failure this guards: qs0/qs1 swapped moves the window up by sixteen
        # bytes. Written as an assertion so a later "cleanup" of the operands is
        # caught here rather than by a wrong audio buffer.
        mem = self._ramp_mem()
        sim = Sim(mem)
        sim.run('ee.ld.128.usar.ip q0, a3, 16\nee.ld.128.usar.ip q1, a3, 16\nee.src.q q2, q1, q0\n',
                {'a3': self.RAMP + 4})
        self.assertNotEqual(sim.q[2], [(mem[self.RAMP + 4 + 2 * i] | (mem[self.RAMP + 5 + 2 * i] << 8))
                                       for i in range(8)])

    def test_vmulas_s16_qacc_is_signed_and_clamped(self):
        mem = bytearray(0x100)
        store16(mem, 0x80, [0x8000, 0x0001, 0x7FFF, 0, 0, 0, 0, 0])   # -32768, 1, 32767
        store16(mem, 0x90, [0x0002, 0xFFFF, 0x7FFF, 0, 0, 0, 0, 0])   # 2, -1, 32767
        sim = Sim(mem)
        sim.run('ee.zero.qacc\nee.vld.128.ip q0, a3, 16\nee.vld.128.ip q1, a4, 16\n'
                'ee.vmulas.s16.qacc q0, q1\n', {'a3': 0x80, 'a4': 0x90})
        self.assertEqual(sim.qacc[:3], [-65536, -1, 32767 * 32767])
        # The clamp is two-sided: an unsigned MAC would have made lane 0 positive.
        sim.run('ee.vmulas.s16.qacc q0, q1\n', {'a3': 0x80, 'a4': 0x90})
        self.assertEqual(sim.qacc[0], -131072)

    def test_st_qacc_l_stores_each_lane_and_srs_accx_shifts_lane_zero(self):
        mem = bytearray(0x200)
        store16(mem, 0xA0, [3, 0xFFFF, 0xFFFE, 4, 5, 6, 7, 8])
        sim = Sim(mem)
        sim.run('ee.vld.128.ip q0, a3, 16\nee.zero.qacc\nee.vmulas.s16.qacc q0, q0\n'
                'ee.st.qacc_l.l.128.ip a4, 16\nee.srs.accx a5, a6, 4\n',
                {'a3': 0xA0, 'a4': 0x180, 'a6': 4})
        # Lane 0 is 3*3 = 9; lane 1 is (-1)*(-1) = 1; lane 2 is (-2)*(-2) = 4.
        self.assertEqual(load16(mem, 0x180, 4)[:3], [9, 1, 4])
        self.assertEqual(sim.ar['a4'], 0x190, 'the QACC store advanced by sixteen')
        self.assertEqual(sim.qacc[0], 9 >> 4, 'SRS.ACCX shifts the accumulator in place')
        self.assertEqual(sim.ar['a5'], 0)


if __name__ == '__main__':
    unittest.main()
