"""The archive-safe proof rejects a mixed or unsupported image pair."""
import hashlib
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

from verify_diagnostic_build import prove


class DiagnosticProofTests(unittest.TestCase):
    def test_actual_pair_and_sdk_required_without_git(self):
        with tempfile.TemporaryDirectory() as temp:
            build = Path(temp)
            elf = b'example ELF'
            (build/'cardputer_pocketjs.elf').write_bytes(elf)
            image = bytearray(288)
            image[0] = 0xE9
            struct.pack_into('<I', image, 32, 0xABCD5432)
            image[144:150] = b'v6.0.1'
            image[176:208] = hashlib.sha256(elf).digest()
            path = build/'cardputer_pocketjs.bin'
            path.write_bytes(image)
            with patch('verify_diagnostic_build.verify_mode', return_value={'telemetry_mode':'ON'}) as mode:
                self.assertEqual(prove(build)['artifacts']['cardputer_pocketjs.bin']['sha256'], hashlib.sha256(image).hexdigest())
                mode.assert_called_once_with(build, 'ON')
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
