import hashlib
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

import verify_derby_runner_batch as proof
from derby_compare_chunks import instrument as compare_view
from derby_runner_batch_chunks import generate as generate_batch
from derby_telemetry_chunks import generate as generate_telemetry


class Proof(unittest.TestCase):
    def test_fixed_view_survives_runner_chunk_composition(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = proof.ROOT / 'apps/derby'
            telemetry, batch = root/'telemetry', root/'batch'
            generate_telemetry(source, telemetry, True)
            generate_batch(telemetry, batch)
            expected = compare_view((source/'derby_view.js').read_text()).encode()
            self.assertEqual((telemetry/'derby_view.js').read_bytes(), expected)
            self.assertIn(str(telemetry/'derby_view.js'), (batch/'chunks.txt').read_text())
            self.assertNotIn(str(source/'derby_view.js'), (batch/'chunks.txt').read_text())

    def fixture(self, root, enabled):
        root.mkdir(exist_ok=True)
        source = (proof.ROOT / 'apps/derby/derby_pan.js').read_bytes()
        generated = proof.instrument(source.decode()).encode()
        target = generated if enabled else source
        (root / 'CMakeCache.txt').write_text('POCKET_DERBY_RUNNER_BATCH:BOOL=' + ('ON' if enabled else 'OFF') + '\n')
        (root / 'cardputer_pocketjs.elf').write_bytes(b'ELF' + target + b'\0')
        image = bytearray(288)
        image[0] = 0xE9
        struct.pack_into('<I', image, 32, 0xABCD5432)
        image[144:152] = b'v6.0.1\0\0'
        image[176:208] = hashlib.sha256((root / 'cardputer_pocketjs.elf').read_bytes()).digest()
        (root / 'cardputer_pocketjs.bin').write_bytes(image)
        if enabled:
            path = root / 'generated/derby_runner_batch/derby_pan.js'
            path.parent.mkdir(parents=True)
            path.write_bytes(generated)

    def test_modes_and_refusals(self):
        for enabled in (False, True):
            with self.subTest(enabled=enabled), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                self.fixture(root, enabled)
                symbols = b'00000001 t js_runners\n' if enabled else b''
                with patch.object(proof.subprocess, 'check_output', return_value=symbols.decode()):
                    mode = 'ON' if enabled else 'OFF'
                    self.assertEqual(proof.verify(root, mode)['native_symbol'], enabled)
                    with self.assertRaises(ValueError):
                        proof.verify(root, 'OFF' if enabled else 'ON')
                    image = root / 'cardputer_pocketjs.bin'
                    good = image.read_bytes()
                    image.write_bytes(good[:176] + b'x' * 32 + good[208:])
                    with self.assertRaises(ValueError): proof.verify(root, mode)
                    image.write_bytes(good)
                    cache = root / 'CMakeCache.txt'
                    good_cache = cache.read_text()
                    cache.write_text(good_cache + good_cache)
                    with self.assertRaises(ValueError): proof.verify(root, mode)
                    cache.write_text(good_cache)
                    path = root / 'generated/derby_runner_batch/derby_pan.js'
                    if enabled:
                        path.write_bytes(b'wrong generated PAN')
                    else:
                        path.parent.mkdir(parents=True); path.write_bytes(b'stale generated PAN')
                    with self.assertRaises(ValueError): proof.verify(root, mode)
                with patch.object(proof.subprocess, 'check_output', return_value='' if enabled else '1 t js_runners'):
                    with self.assertRaises(ValueError): proof.verify(root, mode)


if __name__ == '__main__':
    unittest.main()
