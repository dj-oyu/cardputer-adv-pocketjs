"""The archive-safe proof rejects a mixed or unsupported image pair."""
import hashlib
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

from verify_diagnostic_build import prove
from derby_compare_chunks import instrument as compare_view


class DiagnosticProofTests(unittest.TestCase):
    def test_actual_pair_and_sdk_required_without_git(self):
        with tempfile.TemporaryDirectory() as temp:
            build = Path(temp)
            source_view = (Path(__file__).resolve().parents[1]/'apps/derby/derby_view.js').read_bytes()
            generated_view = compare_view(source_view.decode()).encode()
            generated_path = build/'generated/derby_telemetry/derby_view.js'
            generated_path.parent.mkdir(parents=True)
            generated_path.write_bytes(generated_view)
            elf = b'example ELF' + generated_view + b'\0'
            (build/'cardputer_pocketjs.elf').write_bytes(elf)
            image = bytearray(288)
            image[0] = 0xE9
            struct.pack_into('<I', image, 32, 0xABCD5432)
            image[144:150] = b'v6.0.1'
            image[176:208] = hashlib.sha256(elf).digest()
            path = build/'cardputer_pocketjs.bin'
            path.write_bytes(image)
            (build/'CMakeCache.txt').write_text('POCKET_DERBY_COMPARE_FIXED_HW:BOOL=ON\n')
            with patch('verify_diagnostic_build.verify_mode', return_value={'telemetry_mode':'ON'}) as mode, \
                 patch('verify_diagnostic_build.verify_measure_mode', return_value={'measure_mode':'ON'}) as measure, \
                 patch('verify_diagnostic_build.verify_fixed_mode', return_value={'mode':'OFF'}), \
                 patch('verify_diagnostic_build.symbol_u32', return_value=0x2545F491) as seed:
                self.assertEqual(prove(build)['artifacts']['cardputer_pocketjs.bin']['sha256'], hashlib.sha256(image).hexdigest())
                mode.assert_called_once_with(build, 'ON')
                measure.assert_called_once_with(build, 'ON')
                seed.assert_called_with(elf, 'pocket_derby_comparison_hw_seed')
                self.assertEqual(prove(build)['build_measure']['measure_mode'],'ON')
                self.assertEqual(prove(build)['comparison_seed']['hardware_seed'],'2545F491')
                self.assertEqual(prove(build)['comparison_seed']['saved_race'],19)
                with patch('verify_diagnostic_build.symbol_u32', return_value=0x2545F490), self.assertRaises(ValueError):
                    prove(build)
                generated_path.write_bytes(b'wrong generated view')
                with self.assertRaises(ValueError):
                    prove(build)
                generated_path.write_bytes(generated_view)
                (build/'cardputer_pocketjs.elf').write_bytes(b'example ELF' + source_view + b'\0')
                with self.assertRaises(ValueError):
                    prove(build)
                (build/'cardputer_pocketjs.elf').write_bytes(elf)
                (build/'CMakeCache.txt').write_text('POCKET_DERBY_COMPARE_FIXED_HW:BOOL=OFF\n')
                with self.assertRaises(ValueError):
                    prove(build)
                (build/'CMakeCache.txt').write_text('POCKET_DERBY_COMPARE_FIXED_HW:BOOL=ON\n')
                for offset in (0, 32, 144, 176):
                    changed = image.copy()
                    changed[offset] ^= 1
                    path.write_bytes(changed)
                    with self.subTest(offset=offset), self.assertRaises(ValueError):
                        prove(build)
                path.write_bytes(image)
                (build/'cardputer_pocketjs.elf').write_bytes(b'another ELF')
                with self.assertRaises(ValueError):
                    prove(build)


if __name__ == '__main__':
    unittest.main()
