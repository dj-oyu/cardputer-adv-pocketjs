"""Selection table requires reproducible wins and a verified probe run."""

import json
import tempfile
from pathlib import Path
import unittest

from build_grid_profile import parse_logs, render_profile
from build_grid_measure_profile import select_records, render


class GridProfile(unittest.TestCase):
    def log(self, directory, run, fused, affine, gather=10000, verified=True,
            binary="a" * 64, aligned=None):
        path = directory / f"run-{run}.log"
        path.write_text(
            "KSN_COMPILER: GRID_TIME rev=1 key=0123456789abcdef mask=14 "
            "width=16 height=12 taps_x=2 taps_y=2 calls_per_backend=1024 "
            f"scalar_us=90000 gather_us={gather} affine_us={affine} "
            f"fused_us={fused}\n" +
            ("KSN_COMPILER: GRID_ALIGNED_TIME outputs=192 "
             "calls_per_backend=1024 source_align=0 dest_align=0 "
             f"gather_us={aligned[0]} fused_us={aligned[1]} "
             "key=51df87624d6af998\n" if aligned else "") +
            ("GRID PASS\nKSN_COMPILER: PASS\n" if verified else "GRID FAIL\n") +
            f"GRID_PROFILE_RUN schema=1 run={run:032x} binary={binary} "
            "elf=1234abcd cpu=240000000 idf=v6.0.1 chip=v0.2 target=esp32s3 opt=size\n",
            encoding="utf-8")
        return path

    def test_stable_win_becomes_generated_choice(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            logs = [self.log(root, i, 7600 + i * 20, 10100 + i * 10)
                    for i in range(3)]
            rows = parse_logs(logs)
            self.assertEqual(rows[0][3], "fused")
            self.assertIn("KSN_GRID_PIE_LOAD_FUSED", render_profile(rows))
            self.assertIn('ksn_proc_grid_dual_profile.inc', render_profile(rows))

    def test_small_or_inconsistent_win_keeps_gather(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            logs = [self.log(root, i, fused, 12000)
                    for i, fused in enumerate((9200, 9950, 10100))]
            self.assertEqual(parse_logs(logs)[0][3], "gather")

    def test_rejects_unverified_and_insufficient_runs(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            good = self.log(root, 0, 7000, 11000)
            with self.assertRaises(ValueError):
                parse_logs([good])
            bad = self.log(root, 1, 7000, 11000, verified=False)
            with self.assertRaises(ValueError):
                parse_logs([good, bad, good])

    def test_rejects_duplicate_paths_contents_and_mixed_binary(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            a = self.log(root, 0, 7000, 11000)
            b = self.log(root, 1, 7100, 11000)
            c = self.log(root, 2, 7200, 11000, binary="b" * 64)
            with self.assertRaisesRegex(ValueError, "duplicate log path"):
                parse_logs([a, a, a])
            with self.assertRaisesRegex(ValueError, "different binary"):
                parse_logs([a, b, c])
            copy = root / "copy.log"
            copy.write_bytes(a.read_bytes())
            with self.assertRaisesRegex(ValueError, "duplicate log contents"):
                parse_logs([a, copy, b])

    def test_stable_second_choice_survives_faster_unstable_choice(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            logs = [self.log(root, i, fused, 9000)
                    for i, fused in enumerate((8000, 8000, 10100))]
            self.assertEqual(parse_logs(logs)[0][3], "affine")

    def test_aligned_app_input_selects_fused_and_rejects_wrong_alignment(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            logs = [self.log(root, i, 7600, 10100,
                             aligned=(80000 + i * 10, 61600 + i * 10))
                    for i in range(3)]
            rows = parse_logs(logs)
            aligned = next(row for row in rows if row[0] == 0x51df87624d6af998)
            self.assertEqual(aligned[3], "fused")
            self.assertIn("GRID_ALIGNED_TIME", render_profile(rows))
            logs[0].write_text(logs[0].read_text().replace(
                "source_align=0", "source_align=1"))
            with self.assertRaisesRegex(ValueError, "invalid aligned grid profile"):
                parse_logs(logs)

    def test_dual_measurement_margin_and_provenance(self):
        record = (Path(__file__).resolve().parent / "profiles" /
                  "grid_dual_20260929.json")
        identity, count, rows = select_records([record])
        self.assertEqual(count, 3)
        self.assertEqual([row[-1] for row in rows],
                         ["AFFINE", "AFFINE", "GATHER"])
        self.assertIn(identity[1], render(identity, count, rows))
        with self.assertRaisesRegex(ValueError, "duplicate"):
            select_records([record, record])

    def test_dual_measurement_rejects_mixed_binary(self):
        record = (Path(__file__).resolve().parent / "profiles" /
                  "grid_dual_20260929.json")
        data = json.loads(record.read_text(encoding="utf-8"))
        with tempfile.TemporaryDirectory() as tmp:
            first = Path(tmp) / "first.json"
            second = Path(tmp) / "second.json"
            first.write_text(json.dumps({**data, "runs": data["runs"][:2]}),
                             encoding="utf-8")
            second.write_text(json.dumps({**data, "runs": data["runs"][2:],
                                          "binary_sha256": "0" * 64}),
                              encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "different device or binary"):
                select_records([first, second])


if __name__ == "__main__":
    unittest.main()
