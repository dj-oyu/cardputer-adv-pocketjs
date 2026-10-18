"""Reject stale source labels, mixed ELF/BIN pairs and SDK substitutions."""
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

from record_verification import ROOT, verify_build, verify_mode, TELEMETRY_SYMBOLS
from derby_telemetry_chunks import instrument


class ReceiptTests(unittest.TestCase):
    def test_mode_requires_cache_symbols_exact_chunk_and_actual_elf_bytes(self):
        source=(ROOT/'apps/derby/derby_play.js').read_bytes()
        generated=instrument(source.decode()).encode()
        with tempfile.TemporaryDirectory() as temp:
            build=Path(temp);chunk=build/'generated/derby_telemetry/derby_play.js'
            chunk.parent.mkdir(parents=True)
            cache=build/'CMakeCache.txt';elf=build/'cardputer_pocketjs.elf'
            cache.write_text('POCKET_DERBY_TELEMETRY:BOOL=ON\n')
            chunk.write_bytes(generated);elf.write_bytes(b'ELF'+generated+b'\0')
            with patch('record_verification.elf_symbols',return_value=TELEMETRY_SYMBOLS):
                self.assertEqual(verify_mode(build,'ON')['telemetry_mode'],'ON')
                with self.assertRaises(ValueError):verify_mode(build,'OFF')
                chunk.write_bytes(generated+b'changed')
                with self.assertRaises(ValueError):verify_mode(build,'ON')
                chunk.write_bytes(generated);elf.write_bytes(b'ELF'+source+b'\0')
                with self.assertRaises(ValueError):verify_mode(build,'ON')
                elf.write_bytes(b'ELF'+generated+b'\0'+source+b'\0')
                with self.assertRaises(ValueError):verify_mode(build,'ON')
            elf.write_bytes(b'ELF'+generated+b'\0')
            with patch('record_verification.elf_symbols',return_value=set()):
                with self.assertRaises(ValueError):verify_mode(build,'ON')
                cache.write_text('POCKET_DERBY_TELEMETRY:BOOL=OFF\n')
                elf.write_bytes(b'ELF'+source+b'\0')
                with self.assertRaises(ValueError):verify_mode(build,'OFF') # stale generated file
                chunk.unlink()
                self.assertEqual(verify_mode(build,'OFF')['telemetry_mode'],'OFF')
                elf.write_bytes(b'ELF'+generated+b'\0')
                with self.assertRaises(ValueError):verify_mode(build,'OFF')
            elf.write_bytes(b'ELF'+source+b'\0')
            with patch('record_verification.elf_symbols',return_value=TELEMETRY_SYMBOLS):
                with self.assertRaises(ValueError):verify_mode(build,'OFF')

    def test_receipt_validates_actual_descriptor_and_rejects_relabelling(self):
        head = "123456789abc" + "d" * 28
        with tempfile.TemporaryDirectory() as temp:
            build = Path(temp)
            elf = b"synthetic ELF bytes"
            (build / "cardputer_pocketjs.elf").write_bytes(elf)
            description = dict(project_path=str(ROOT), project_version=head[:12])
            (build / "project_description.json").write_text(json.dumps(description))
            image = bytearray(288)
            image[0] = 0xE9
            struct.pack_into("<I", image, 32, 0xABCD5432)
            image[48:60] = head[:12].encode()
            image[144:150] = b"v6.0.1"
            image[176:208] = hashlib.sha256(elf).digest()
            path = build / "cardputer_pocketjs.bin"
            path.write_bytes(image)
            verify_build(build, head)
            for offset in [0, 32, 48, 144, 176]:
                changed = image.copy()
                changed[offset] ^= 1
                path.write_bytes(changed)
                with self.subTest(offset=offset), self.assertRaises(ValueError):
                    verify_build(build, head)
            path.write_bytes(image)
            (build / "cardputer_pocketjs.elf").write_bytes(b"another ELF")
            with self.assertRaises(ValueError):
                verify_build(build, head)
            (build / "cardputer_pocketjs.elf").write_bytes(elf)
            description["project_version"] = "stale-version"
            (build / "project_description.json").write_text(json.dumps(description))
            with self.assertRaises(ValueError):
                verify_build(build, head)


if __name__ == "__main__":
    unittest.main()
