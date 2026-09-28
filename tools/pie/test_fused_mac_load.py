"""TRM 1.8.140-145 fused signed QACC MAC/load instructions."""
import unittest

from piesim import Sim, store16


class FusedMacLoad(unittest.TestCase):
    def setUp(self):
        self.mem = bytearray(0x400)
        store16(self.mem, 0x40, [0xFFFF, 2, 0x8000, 0x7FFF, 5, 6, 7, 8])
        store16(self.mem, 0x80, [9, 10, 11, 12, 13, 14, 15, 16])
        store16(self.mem, 0x90, [21, 22, 23, 24, 25, 26, 27, 28])
        store16(self.mem, 0xA0, [31, 32, 33, 34, 35, 36, 37, 38])

    def initial(self):
        sim = Sim(bytearray(self.mem))
        sim.q[0] = sim.ldq(0x40)
        sim.q[1] = [0xFFFF, 0xFFFF, 0x8000, 0x7FFF, 3, 4, 5, 6]
        sim.q[2] = [0xBEEF] * 8
        sim.qacc = [(1 << 39) - 1, -(1 << 39), 123, -123, 0, 0, 0, 0]
        return sim

    def check_against_scalar(self, fused, scalar, ar):
        a, b = self.initial(), self.initial()
        a.run(fused, ar)
        b.run(scalar, ar)
        self.assertEqual(a.qacc, b.qacc)
        self.assertEqual(a.q, b.q)
        self.assertEqual(a.ar, b.ar)
        self.assertEqual(a.sar_byte, b.sar_byte)
        # Positive and negative QACC saturation, and signed multiplication.
        self.assertEqual(a.qacc[0], (1 << 39) - 1)
        self.assertEqual(a.qacc[1], -(1 << 39))

    def test_ip_load_before_pointer_advance_and_source_before_destination(self):
        self.check_against_scalar(
            'ee.vmulas.s16.qacc.ld.ip q0, p, 16, q0, q1',
            'ee.vmulas.s16.qacc q0, q1\nee.vld.128.ip q0, p, 16',
            {'p': 0x83})
        self.check_against_scalar(
            'ee.vmulas.s16.qacc.ld.ip q2, p, -16, q0, q1',
            'ee.vmulas.s16.qacc q0, q1\nee.vld.128.ip q2, p, -16',
            {'p': 0x93})

    def test_ip_immediate_boundaries(self):
        for inc in (-512, 0, 496):
            with self.subTest(inc=inc):
                a = self.initial()
                a.run(f'ee.vmulas.s16.qacc.ld.ip q2, p, {inc}, q0, q1', {'p': 0x80})
                self.assertEqual(a.ar['p'], 0x80 + inc)
        for inc in (-528, 1, 512):
            with self.subTest(invalid=inc):
                with self.assertRaises(ValueError):
                    self.initial().run(f'ee.vmulas.s16.qacc.ld.ip q2, p, {inc}, q0, q1', {'p': 0x80})

    def test_xp_uses_register_increment_unmodified(self):
        for delta in (-32, 3, 48):
            with self.subTest(delta=delta):
                self.check_against_scalar(
                    'ee.vmulas.s16.qacc.ld.xp q2, p, d, q0, q1',
                    f'ee.vmulas.s16.qacc q0, q1\nee.vld.128.ip q2, p, {delta}',
                    {'p': 0x93, 'd': delta})

    def test_ldbc_forces_halfword_alignment_and_increments_two(self):
        self.check_against_scalar(
            'ee.vmulas.s16.qacc.ldbc.incp q1, p, q0, q1',
            'ee.vmulas.s16.qacc q0, q1\nee.vldbc.16.ip q1, p, 2',
            {'p': 0x91})

    def test_ip_qup_uses_old_sources_and_sar_byte(self):
        a, b = self.initial(), self.initial()
        a.q[3], a.q[4] = a.ldq(0x80), a.ldq(0x90)
        b.q[3], b.q[4] = b.ldq(0x80), b.ldq(0x90)
        a.sar_byte = b.sar_byte = 3
        a.run('ee.vmulas.s16.qacc.ld.ip.qup q2, p, 16, q0, q1, q3, q4', {'p': 0xA3})
        b.run('ee.vmulas.s16.qacc q0, q1\nee.vld.128.ip q2, p, 16\nee.src.q q3, q3, q4',
              {'p': 0xA3})
        self.assertEqual((a.qacc, a.q, a.ar), (b.qacc, b.q, b.ar))
        self.assertEqual(a.q[3][0], 0x0B00)  # bytes 3 and 4 of the 0x80..0x9f window
        self.assertEqual(a.sar_byte, 3)  # QUP consumes SAR_BYTE; it does not set it


if __name__ == '__main__':
    unittest.main()
