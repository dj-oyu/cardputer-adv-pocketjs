"""Pipeline dependencies for TRM table 1.7-2 signed QACC fused loads."""
import unittest

from stalls import FUSED_MAC_LOAD, MEMORY, analyse, operands, parse


class FusedMacLoadStalls(unittest.TestCase):
    def test_all_fused_forms_are_memory_with_stage_two_qr_result(self):
        for op in FUSED_MAC_LOAD:
            with self.subTest(op=op):
                self.assertIn(op, MEMORY)
                defs, uses = operands(op, ['q0', 'q1', 'q2', 'q3', 'q4'])
                self.assertIn(('q0', 2), defs)
                self.assertIn('q1', uses)
                self.assertIn('q2', uses)
        self.assertEqual(operands('ee.vmulas.s16.qacc.ld.ip.qup',
                                  ['q0', 'q1', 'q2', 'q3', 'q4']),
                         ([('q3', 1), ('q0', 2)], ['q1', 'q2', 'q3', 'q4']))

    def test_loaded_qr_immediately_consumed_interlocks(self):
        body = parse('ee.vmulas.s16.qacc.ld.ip q0, p, 16, q1, q2\n'
                     'ee.vmulas.s16.qacc q0, q2\n'
                     'ee.zero.q q7\n')
        self.assertIn((0, 'ee.vmulas.s16.qacc.ld.ip', 1,
                       'ee.vmulas.s16.qacc', 'q0'), analyse(body))

    def test_self_update_reads_prior_value_and_defines_new_value(self):
        body = parse('ee.vld.128.ip q0, p, 16\n'
                     'ee.vmulas.s16.qacc.ld.xp q0, p, d, q0, q2\n'
                     'ee.vmulas.s16.qacc q0, q2\n'
                     'ee.zero.q q7\n')
        self.assertEqual(analyse(body), [
            (0, 'ee.vld.128.ip', 1, 'ee.vmulas.s16.qacc.ld.xp', 'q0'),
            (1, 'ee.vmulas.s16.qacc.ld.xp', 2, 'ee.vmulas.s16.qacc', 'q0')])

    def test_qup_stage_one_output_does_not_cause_load_stall(self):
        body = parse('ee.vmulas.s16.qacc.ld.ip.qup q0, p, 16, q1, q2, q3, q4\n'
                     'ee.vmulas.s16.qacc q3, q2\n'
                     'ee.zero.q q7\n')
        self.assertEqual(analyse(body), [])


if __name__ == '__main__':
    unittest.main()
