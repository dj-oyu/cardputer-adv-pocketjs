"""Reject mislabeled DMA images and untyped or ambiguous requests offline."""
import hashlib
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from verify_dma_diagnostic_build import CONTRACT, DMA_SYMBOLS, prove_dma


class DmaDiagnosticProofTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.build = Path(self.temp.name)

    def cache(self, dma, detail):
        (self.build / 'CMakeCache.txt').write_text(
            'KASANE_DMA_OVERLAP:BOOL=' + dma + '\nPOCKET_DERBY_DETAIL_MEASURE:BOOL=' + detail + '\n')

    def prove(self, dma='OFF', detail='OFF', symbols=(), verified_detail=None, error=None):
        diagnostic = dict(schema_version=1, detail_measurement=dict(mode=verified_detail or detail),
                          artifacts={'cardputer_pocketjs.elf': dict(bytes=3, sha256='a' * 64)},
                          hardware_tested=False)
        with patch('verify_dma_diagnostic_build.prove', return_value=diagnostic, side_effect=error), \
             patch('verify_dma_diagnostic_build.elf_symbols', return_value=set(symbols)), \
             patch('verify_dma_diagnostic_build.dma_function_symbols', return_value=set(symbols)):
            return prove_dma(self.build, dma_overlap=dma, detail_measurement=detail)

    def test_all_four_axes_and_actual_hash_binding(self):
        for dma in ('OFF', 'ON'):
            for detail in ('OFF', 'ON'):
                with self.subTest(dma=dma, detail=detail):
                    self.cache(dma, detail)
                    result = self.prove(dma, detail, DMA_SYMBOLS if dma == 'ON' else ())
                    self.assertEqual(result['proof_contract'], CONTRACT)
                    self.assertEqual(result['requested_axes'], result['verified_axes'])
                    self.assertEqual(result['artifacts'], result['diagnostic']['artifacts'])
                    self.assertEqual(result['dma_overlap']['cmake_cache_sha256'],
                                     hashlib.sha256((self.build / 'CMakeCache.txt').read_bytes()).hexdigest())

    def test_missing_ambiguous_invalid_or_mismatched_cache_fails(self):
        for text in ('', 'KASANE_DMA_OVERLAP:BOOL=OFF\n',
                     'KASANE_DMA_OVERLAP:BOOL=ON\nPOCKET_DERBY_DETAIL_MEASURE:BOOL=OFF\n',
                     'KASANE_DMA_OVERLAP:BOOL=FALSE\nPOCKET_DERBY_DETAIL_MEASURE:BOOL=OFF\n',
                     'KASANE_DMA_OVERLAP:BOOL=OFF\nKASANE_DMA_OVERLAP:BOOL=OFF\nPOCKET_DERBY_DETAIL_MEASURE:BOOL=OFF\n',
                     'KASANE_DMA_OVERLAP:BOOL=OFF\nPOCKET_DERBY_DETAIL_MEASURE:BOOL=OFF\nPOCKET_DERBY_DETAIL_MEASURE:BOOL=OFF\n',
                     'KASANE_DMA_OVERLAP:BOOL=OFF\nPOCKET_DERBY_DETAIL_MEASURE:BOOL=ON\n'):
            with self.subTest(cache=text):
                (self.build / 'CMakeCache.txt').write_text(text)
                with self.assertRaises(ValueError):
                    self.prove()

    def test_each_partial_or_wrong_linked_dma_mode_fails(self):
        for dma, symbols in (('ON', set()), ('ON', {'board_present_submit'}),
                             ('ON', {'board_present_finish'}), ('OFF', DMA_SYMBOLS),
                             ('OFF', {'board_present_submit'}), ('OFF', {'board_present_finish'})):
            with self.subTest(dma=dma, symbols=symbols):
                self.cache(dma, 'OFF')
                with self.assertRaises(ValueError):
                    self.prove(dma, symbols=symbols)

    def test_unknown_axes_and_detail_proof_mismatch_fail(self):
        self.cache('OFF', 'OFF')
        for value in (None, False, True, 0, 1, '', 'off', 'AUTO'):
            for axis in ('dma', 'detail'):
                with self.subTest(value=value, axis=axis), self.assertRaises(ValueError):
                    self.prove(**{axis: value})
        with self.assertRaises(ValueError):
            self.prove(verified_detail='ON')

    def test_shadow_cache_types_are_not_ignored(self):
        for key in ('KASANE_DMA_OVERLAP', 'POCKET_DERBY_DETAIL_MEASURE'):
            for suffix in (':STRING=OFF', '=OFF', ':INTERNAL=OFF'):
                self.cache('OFF', 'OFF')
                with (self.build / 'CMakeCache.txt').open('a') as cache:
                    cache.write(key + suffix + '\n')
                with self.subTest(key=key, suffix=suffix), self.assertRaises(ValueError):
                    self.prove()

    def test_data_symbols_cannot_stand_in_for_dma_functions(self):
        from verify_dma_diagnostic_build import dma_function_symbols
        for kind in ('B', 'D', 'R', 'A'):
            text = '\n'.join('00000100 ' + kind + ' ' + name for name in DMA_SYMBOLS)
            with patch('verify_dma_diagnostic_build.command', return_value=text):
                self.assertFalse(DMA_SYMBOLS & dma_function_symbols(self.build))

    def test_existing_diagnostic_failure_is_not_bypassed(self):
        self.cache('OFF', 'OFF')
        with self.assertRaisesRegex(ValueError, 'mismatched BIN/ELF'):
            self.prove(error=ValueError('mismatched BIN/ELF'))


if __name__ == '__main__':
    unittest.main()
