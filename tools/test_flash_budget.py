"""Regression checks for the SKK/font Flash reservation guard."""
from pathlib import Path
import tempfile
import unittest
from check_flash import check

class FlashBudgetTest(unittest.TestCase):
    def test_reservation_and_overflow(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'app.bin'
            binary.write_bytes(b'hello')
            check(binary)
            with binary.open('wb') as f:
                f.truncate(0x300001)
            with self.assertRaises(SystemExit):
                check(binary)

    def test_reject_shrunken_dictionary(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binary = root / 'app.bin'
            binary.write_bytes(b'hello')
            csv = root / 'partitions.csv'
            source = Path(__file__).resolve().parents[1] / 'partitions.csv'
            csv.write_text(source.read_text().replace('0x200000', '0x100000', 1))
            with self.assertRaises(SystemExit):
                check(binary, csv)

if __name__ == '__main__':
    unittest.main()
