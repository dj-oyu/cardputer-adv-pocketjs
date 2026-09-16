"""Every parallel compiler failure must prevent linking, even with cached objects."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
UNITS = ("dtoa", "libregexp", "libunicode", "quickjs", "quickjs-libc", "quickjs-vm")


class BuildFailureTest(unittest.TestCase):
    def test_parallel_compilers(self):
        for failed in (*UNITS, "none"):
            with self.subTest(failed=failed), tempfile.TemporaryDirectory() as tmp:
                out = Path(tmp)
                obj = out / "obj-o2"
                obj.mkdir()
                for name in UNITS:
                    path = obj / (name + ".o")
                    path.write_text("stale object")
                    os.utime(path, (1, 1))
                compiler = out / "gcc"
                compiler.write_text(
                    '#!/bin/bash\n'
                    'for arg in "$@"; do\n'
                    '  if [[ "$arg" == */"$VMTEST_FAILED_UNIT.c" ]]; then exit 7; fi\n'
                    'done\n'
                    'if [[ " $* " != *" -c "* ]]; then touch "$VMTEST_LINK_MARK"; fi\n'
                    'exit 0\n')
                compiler.chmod(0o755)
                mark = out / "linked"
                env = dict(os.environ, PATH=tmp + os.pathsep + os.environ["PATH"],
                           VMTEST_OUT=tmp, VMTEST_FAILED_UNIT=failed, VMTEST_LINK_MARK=str(mark))
                result = subprocess.run(["bash", "tools/vmtest/build.sh", "o2"],
                                        cwd=ROOT, env=env, capture_output=True, text=True, timeout=20)
                self.assertEqual(result.returncode, 0 if failed == "none" else 1, result.stderr)
                self.assertEqual(mark.exists(), failed == "none", result.stdout)


if __name__ == "__main__":
    unittest.main()
