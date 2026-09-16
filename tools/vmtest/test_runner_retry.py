"""Exercise the real corpus driver's retry classification with fake runners."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class RetryTest(unittest.TestCase):
    def test_only_pre_main_asan_failures_retry(self):
        for variant, started, signature, expected in (
                ("asan-fake", False, False, 2),
                ("asan-fake", False, True, 2),
                ("asan-fake", True, False, 1),
                ("asan-fake", True, True, 1),
                ("o2-fake", False, False, 1)):
            with self.subTest(variant=variant, started=started, signature=signature), tempfile.TemporaryDirectory() as tmp:
                out = Path(tmp)
                runner = out / ("vmrun-" + variant)
                counter = out / "counter"
                runner.write_text("#!/bin/sh\n"
                                  '[ "$VMTEST_START_MARKER" = 1 ] || exit 64\n'
                                  'printf x >> "$VMTEST_RETRY_COUNTER"\n'
                                  + ('printf "#info vmrun-start\\n" >&2\n' if started else '')
                                  + ('printf "AddressSanitizer:DEADLYSIGNAL\\n" >&2\n' if signature else '')
                                  + 'exit 124\n')
                runner.chmod(0o755)
                env = dict(os.environ, VMTEST_OUT=tmp, VMTEST_HANG_RETRIES="2",
                           VMTEST_RETRY_COUNTER=str(counter), VMTEST_VMRUN_FLAGS="")
                result = subprocess.run(["bash", "tools/vmtest/run.sh", "--variant", variant, "lazy_call_inputs"],
                                        cwd=ROOT, env=env, capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 1)
                self.assertEqual(len(counter.read_text()), expected)
                self.assertIn("exit=124", (out / ("actual-" + variant) / "lazy_call_inputs.raw").read_text())


if __name__ == "__main__":
    unittest.main()
