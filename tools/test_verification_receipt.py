"""Reject stale source labels, mixed ELF/BIN pairs and SDK substitutions."""
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest

from record_verification import ROOT, verify_build


class ReceiptTests(unittest.TestCase):
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
