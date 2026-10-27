"""Reject old/incomplete resource evidence instead of claiming SRAM savings."""
import unittest
from report_board_transport import parse_symbols, report

SYMBOLS = '''
3fc80000 00000f00 b shared
3fc80f00 00000f00 b tx_buf
3fc81e00 00000002 b window_x
3fc81e02 00000002 b window_cols
3fc81e10 00000044 b tx_pending
40370000 00000040 t board_present
'''


class ResourceProof(unittest.TestCase):
    def evidence(self, text=SYMBOLS):
        return report(parse_symbols(text), 'a'*40, 'b'*64)

    def test_current_named_storage_is_bound_to_source_and_elf(self):
        proof = self.evidence()
        self.assertEqual(proof['named_storage_bytes'], 7684)
        self.assertEqual(proof['named_storage_delta_bytes'], -3840)
        self.assertEqual(proof['source_head'], 'a'*40)
        self.assertEqual(proof['elf_sha256'], 'b'*64)

    def test_old_two_tx_payload_cannot_claim_single_tx_savings(self):
        with self.assertRaises(ValueError):
            self.evidence(SYMBOLS.replace('00000f00 b tx_buf', '00001e00 b tx_buf'))
        with self.assertRaises(ValueError):
            self.evidence(SYMBOLS+'3fc82000 00000004 b tx_front\n')

    def test_missing_or_text_symbols_cannot_be_counted_as_ram(self):
        with self.assertRaises(ValueError):
            self.evidence(SYMBOLS.replace('3fc81e00 00000002 b window_x\n', ''))
        with self.assertRaises(ValueError):
            self.evidence(SYMBOLS.replace('00000f00 b tx_buf', '00000f00 t tx_buf'))

    def test_duplicate_or_invalid_source_evidence_is_rejected(self):
        with self.assertRaises(ValueError):
            self.evidence(SYMBOLS+'3fc83000 00000f00 b tx_buf\n')
        with self.assertRaises(ValueError):
            report(parse_symbols(SYMBOLS), 'moving-branch-name', 'b'*64)


if __name__ == '__main__':
    unittest.main()
