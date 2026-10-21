"""Compiler cache acceptance: dependencies, variants, contention and fresh links."""
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import host_build as hb


class HostBuildTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.cache = patch.object(hb, 'CACHE', self.root / 'objects')
        self.cache.start()
        self.addCleanup(self.cache.stop)
        self.header = self.root / 'value.h'
        self.header.write_text('#define VALUE 1\n')
        self.source = self.root / 'value.c'
        self.source.write_text('#include <value.h>\nint value(void) { return VALUE; }\n')
        self.flags = ['-O2', '-I', str(self.root)]

    def obj(self, flags=None):
        return hb.compile_object('cc', self.source, self.flags if flags is None else flags)

    def test_same_inputs_reuse_and_preserved_timestamp_header_edit_invalidates(self):
        obj = self.obj()
        original, stamp = obj.read_bytes(), obj.stat().st_mtime_ns
        self.assertEqual(self.obj().stat().st_mtime_ns, stamp)
        old = self.header.stat()
        self.header.write_text('#define VALUE 2\n')
        os.utime(self.header, ns=(old.st_atime_ns, old.st_mtime_ns))
        self.assertNotEqual(self.obj().read_bytes(), original)

    def test_new_shadow_header_is_rescanned(self):
        first = self.root / 'first'
        first.mkdir()
        flags = ['-O2', '-I', str(first), '-I', str(self.root)]
        before = self.obj(flags).read_bytes()
        (first / 'value.h').write_text('#define VALUE 9\n')
        self.assertNotEqual(self.obj(flags).read_bytes(), before)

    def test_flags_and_abi_have_separate_objects(self):
        variants = [self.obj(self.flags + extra) for extra in (
            [], ['-fsanitize=address,undefined'], ['-DFEATURE=1'], ['-m32'])]
        self.assertEqual(len(set(variants)), 4)
        self.assertEqual(variants[-1].read_bytes()[4], 1)  # ELFCLASS32
        self.assertEqual(variants[0].read_bytes()[4], 2)  # ELFCLASS64

    def test_corrupt_object_rebuilds_and_parallel_requests_share_one_object(self):
        original = self.obj().read_bytes()
        self.obj().write_bytes(b'bad cached output')
        with ThreadPoolExecutor(max_workers=4) as pool:
            objects = list(pool.map(lambda _: self.obj(), range(4)))
        self.assertEqual(len(set(objects)), 1)
        self.assertEqual(objects[0].read_bytes(), original)

    def test_corrupt_metadata_is_a_miss_and_rebuilt_atomically(self):
        obj = self.obj()
        original = obj.read_bytes()
        metadata = obj.parent / 'inputs.json'
        for broken in ('', '{"inputs":', '[]', '{}'):
            with self.subTest(metadata=broken):
                metadata.write_text(broken)
                self.assertEqual(self.obj().read_bytes(), original)
                import json
                rebuilt = json.loads(metadata.read_text())
                self.assertEqual(rebuilt['object'], hb.digest(obj))
                self.assertIn(str(self.header), rebuilt['inputs'])

    def test_every_build_relinks_and_link_failure_propagates(self):
        main = self.root / 'main.c'
        main.write_text('int value(void); int main(void) { return value() != 1; }\n')
        binary = self.root / 'test'
        command = ['cc', *self.flags, str(main), str(self.source), '-o', str(binary)]
        hb.build(command)
        subprocess.run([str(binary)], check=True)
        binary.write_text('stale executable')
        hb.build(command)
        subprocess.run([str(binary)], check=True)
        with self.assertRaises(subprocess.CalledProcessError):
            hb.build(command + ['-lmissing_ci_test_library'])

    def test_missing_dependency_fails_instead_of_reusing(self):
        self.obj()
        self.header.unlink()
        with self.assertRaises(subprocess.CalledProcessError):
            self.obj()


if __name__ == '__main__':
    unittest.main()
