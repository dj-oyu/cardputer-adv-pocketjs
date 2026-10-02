"""Manifest safety tests plus optional exact-engine compiler round trips.

python tools/test_precompile_apps.py --cc /path/to/gcc
python tools/test_precompile_apps.py --cc /path/to/gcc --sanitize
python tools/test_precompile_apps.py --cc /path/to/gcc --functional-only
Add --functional-only for ordinary paths without low-heap or allocator-fault tests.
No target/device access. Temporary sources and outputs are isolated.
"""
import argparse
import copy
import json
import os
import re
import sys
import shutil
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import precompile_apps as bc

CC = None
FUNCTIONAL_ONLY = False


class ManifestTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.src = self.root / 'fixture.js'
        self.src.write_text('globalThis.answer = 42;\n')
        self.out = self.root / 'fixture.js.bc'
        self.out.write_bytes(b'bytecode fixture')
        self.m = {'format': 1, 'experimental_only': True, 'kind': 'global',
                  'filename': self.src.name, 'source_sha256': bc.digest(self.src),
                  'source_bytes': self.src.stat().st_size,
                  'bytecode_sha256': bc.digest(self.out),
                  'bytecode_bytes': self.out.stat().st_size,
                  'compiler': {'engine': bc.fingerprint()},
                  'abi': {'pointer_bytes': 8, 'jsvalue_bytes': 16}}
        self.write()

    def write(self):
        Path(str(self.out) + '.json').write_text(json.dumps(self.m))

    def rejected(self):
        self.write()
        with self.assertRaises(ValueError):
            bc.verify(self.src, self.out)

    def test_valid(self):
        self.assertEqual(bc.verify(self.src, self.out), self.m)

    def test_stale_source(self):
        self.src.write_text('globalThis.answer = 43;\n')
        self.rejected()

    def test_corrupt_bytecode(self):
        self.out.write_bytes(b'corrupt bytecode!')
        self.rejected()

    def test_truncated_bytecode(self):
        self.out.write_bytes(self.out.read_bytes()[:-1])
        self.rejected()

    def test_length_mismatch(self):
        self.m['bytecode_bytes'] += 1
        self.rejected()

    def test_source_length_mismatch(self):
        self.m['source_bytes'] += 1
        self.rejected()

    def test_engine_mismatch(self):
        self.m['compiler']['engine'] = 'old engine'
        self.rejected()

    def test_filename_mismatch(self):
        self.m['filename'] = 'different.js'
        self.rejected()

    def test_module_kind(self):
        self.m['kind'] = 'module'
        self.rejected()

    def test_module_extension(self):
        src = self.src.rename(self.src.with_suffix('.mjs'))
        self.m['filename'] = src.name
        self.write()
        with self.assertRaises(ValueError):
            bc.verify(src, self.out)

    def test_format_mismatch(self):
        self.m['format'] = 2
        self.rejected()

    def test_not_experimental(self):
        self.m['experimental_only'] = False
        self.rejected()

    def test_target_abi_mismatch(self):
        with self.assertRaises(ValueError):
            bc.verify(self.src, self.out, {'pointer_bytes': 4, 'jsvalue_bytes': 8})

    def test_flag_identity(self):
        self.assertNotEqual(bc.fingerprint(), bc.fingerprint([*bc.FLAGS, '-DOTHER']))

    def test_failed_compile_preserves_existing(self):
        before = self.out.read_bytes()
        with patch.object(bc.subprocess, 'check_output', return_value='{}'), \
             patch.object(bc.subprocess, 'run', side_effect=subprocess.CalledProcessError(1, 'compiler')):
            with self.assertRaises(subprocess.CalledProcessError):
                bc.generate(self.src, self.out, Path('unused'), {})
        self.assertEqual(self.out.read_bytes(), before)
        self.assertEqual(bc.verify(self.src, self.out), self.m)

    def test_interrupted_pair_is_detected(self):
        # A process can stop between the two atomic renames. The surviving
        # old manifest must never validate the newly published bytecode.
        original_replace = os.replace
        def replace(source, destination):
            if str(destination).endswith('.json'):
                raise OSError('simulated interruption publishing manifest')
            original_replace(source, destination)
        def compile_output(args, **kwargs):
            Path(args[-1]).write_bytes(b'replacement bytecode')
        with patch.object(bc.subprocess, 'check_output', return_value='{}'), \
             patch.object(bc.subprocess, 'run', side_effect=compile_output), \
             patch.object(bc.os, 'replace', side_effect=replace):
            with self.assertRaises(OSError):
                bc.generate(self.src, self.out, Path('unused'), {})
        with self.assertRaisesRegex(ValueError, 'bytecode hash/length mismatch'):
            bc.verify(self.src, self.out)
        self.assertFalse(list(self.root.glob('*.source.*')))

    def test_no_timestamp_churn(self):
        before = self.out.stat().st_mtime_ns
        bc.atomic_write(self.out, self.out.read_bytes())
        self.assertEqual(self.out.stat().st_mtime_ns, before)

    def test_source_edit_during_compile_rejected(self):
        before = self.out.read_bytes()
        def edit(args, **kwargs):
            self.src.write_text('changed during compile')
            Path(args[-1]).write_bytes(b'old-source-bytecode')
        with patch.object(bc.subprocess, 'check_output', return_value='{}'), \
             patch.object(bc.subprocess, 'run', side_effect=edit):
            with self.assertRaisesRegex(ValueError, 'source changed'):
                bc.generate(self.src, self.out, Path('unused'), {})
        self.assertEqual(self.out.read_bytes(), before)


@unittest.skipUnless(CC, 'native compiler integration requested with --cc')
class CompilerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        cls.compiler, cls.identity = bc.build_compiler(cls.root / 'compiler', CC)
        cls.runner = cls.root / 'eval.exe'
        subprocess.run([CC, *bc.FLAGS, '-Wall', '-Wextra', '-Werror',
                        '-I', str(bc.QJS), str(bc.ROOT / 'tools/test_precompile_eval.c'),
                        *[str(cls.root / 'compiler' / (n + '.o')) for n in bc.SOURCES],
                        '-lm', '-o', str(cls.runner)], check=True, env=bc.compiler_env(CC))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_unchanged_compiler_is_reused(self):
        before = self.compiler.stat().st_mtime_ns
        compiler, identity = bc.build_compiler(self.root / 'compiler', CC)
        self.assertEqual(compiler, self.compiler)
        self.assertEqual(identity, self.identity)
        self.assertEqual(compiler.stat().st_mtime_ns, before)

    def test_reproducible_regeneration(self):
        source = self.root / 'closure.js'
        source.write_text('function outer(x) { return y => x + y; }\nlet f = outer(7);\n')
        out = self.root / 'closure.bc'
        bc.generate(source, out, self.compiler, self.identity)
        before = out.read_bytes()
        m = copy.deepcopy(bc.verify(source, out))
        bc.generate(source, out, self.compiler, self.identity)
        self.assertEqual(before, out.read_bytes())
        self.assertEqual(m, bc.verify(source, out))
        source.write_text(source.read_text() + 'globalThis.answer = f(35);\n')
        bc.generate(source, out, self.compiler, self.identity)
        self.assertNotEqual(before, out.read_bytes())
        bc.verify(source, out)

    def test_compile_does_not_execute(self):
        source = self.root / 'throw.js'
        source.write_text('throw new Error("not executed while compiling");\n')
        bc.generate(source, self.root / 'throw.bc', self.compiler, self.identity)

    def test_syntax_error_no_output(self):
        source = self.root / 'bad.js'
        source.write_text('function (\n')
        out = self.root / 'bad.bc'
        with self.assertRaises(subprocess.CalledProcessError):
            bc.generate(source, out, self.compiler, self.identity)
        self.assertFalse(out.exists())
        self.assertFalse(Path(str(out) + '.json').exists())

    def test_module_rejected(self):
        source = self.root / 'module.mjs'
        source.write_text('export const answer = 42;\n')
        with self.assertRaises(ValueError):
            bc.generate(source, self.root / 'module.bc', self.compiler, self.identity)

    def evaluate(self, mode, path, filename, limit=10_000_000, fail_after=None):
        if FUNCTIONAL_ONLY and (limit != 10_000_000 or fail_after is not None):
            raise ValueError('--functional-only forbids heap pressure and allocator faults')
        args = [str(self.runner), mode, str(path), filename, str(limit)]
        if fail_after is not None:
            args.append(str(fail_after))
        p = subprocess.run(args,
                           capture_output=True, text=True, timeout=15, env=bc.compiler_env(CC))
        self.assertIn(p.returncode, (0, 1), p.stderr)
        return p.returncode, p.stdout

    def test_source_bytecode_semantics(self):
        fixtures = [
            ('function f(x){return y=>x+y;} f(7)(35);', '42'),
            ('function f(){let a=3; return eval("a+39");} f();', '42'),
            ('let a=new Int16Array([32768,-32769,42]); JSON.stringify(Array.from(a));',
             '[-32768,32767,42]'),
            ('const a=[1,,3]; JSON.stringify([a.length,1 in a,a.map(x=>x*2)]);',
             '[3,false,[2,null,6]]'),
            ('JSON.stringify([Object.is(-0, -1/Infinity), 1/0 === Infinity, Number.isNaN(NaN)]);',
             '[true,true,true]'),
            ('JSON.stringify(["日本語😀".length,/a+/g.test("caa"),[..."😀"]]);',
             '[5,true,["😀"]]'),
            ('function f(){try{return x;}catch(e){return e.name;} let x=1;} f();',
             'ReferenceError'),
            ('function* f(){yield 1; yield* [2,3]; return 4;} JSON.stringify([...f()]);',
             '[1,2,3]'),
            ('class A {m(){return 40}} class B extends A {m(){return super.m()+2}} new B().m();',
             '42'),
            ('function F(){return new.target===F?{answer:42}:{answer:0};} new F().answer;',
             '42'),
            ('let {x=7,...rest}={y:6}; let a=[1,2]; JSON.stringify([x*rest.y,...a]);',
             '[42,1,2]'),
            ('let o={深い:{値:42}}; o?.深い?.値 ?? 0;', '42'),
            ('String((2n**80n)+42n);', '1208925819614629174706218'),
            ('const b=new ArrayBuffer(8); const d=new DataView(b); '
             'd.setFloat64(0, Math.PI, true); d.getFloat64(0, true)===Math.PI;', 'true'),
        ]
        for i, (text, expected) in enumerate(fixtures):
            with self.subTest(i=i):
                source = self.root / ('semantics%d.js' % i)
                source.write_text(text, encoding='utf-8')
                out = self.root / ('semantics%d.bc' % i)
                bc.generate(source, out, self.compiler, self.identity)
                source_result = self.evaluate('src', source, source.name)
                self.assertEqual(source_result, (0, 'RESULT ' + expected + '\n'))
                self.assertEqual(source_result, self.evaluate('bc', out, source.name))

    def test_error_file_and_line_preserved(self):
        source = self.root / 'position.js'
        source.write_text('function f(){\n\n throw new Error("position");\n}\nf();\n')
        out = self.root / 'position.bc'
        bc.generate(source, out, self.compiler, self.identity)
        actual = self.evaluate('bc', out, source.name)
        self.assertEqual(self.evaluate('src', source, source.name), actual)
        self.assertIn('position.js:3', actual[1])

    def test_low_heap_read_cleanup_and_recovery(self):
        if FUNCTIONAL_ONLY:
            self.skipTest('--functional-only excludes low-heap/OOM evaluation')
        source = self.root / 'heap.js'
        source.write_text('function f(x){return ()=>x+1;} let a=f(41); a();')
        out = self.root / 'heap.bc'
        bc.generate(source, out, self.compiler, self.identity)
        # Independent fresh realms prevent a failed test from polluting the
        # next one. This is limit-based OOM, not arbitrary fail-N injection.
        for repeat in range(3):
            for limit in (0, 1, 1000, 10000, 100000, 1000000):
                with self.subTest(repeat=repeat, limit=limit):
                    self.evaluate('bc', out, source.name, limit)
            self.assertEqual(self.evaluate('bc', out, source.name)[0], 0)

    @unittest.skipUnless(shutil.which('node'), 'Node is required to lower production plans')
    def test_production_lowered_chunks(self):
        # Exercise exactly the sources selected by the shipping chunk-table
        # generator, including ROM-plan removal before bytecode compilation.
        generated = self.root / 'production'
        lists = sorted((bc.ROOT / 'apps').glob('*/chunks.txt'))
        subprocess.run([sys.executable, str(bc.ROOT / 'tools/make_app_chunks.py'),
                        str(generated), *map(str, lists)], check=True)
        text = (generated / 'app_chunks.cmake').read_text()
        def values(name):
            match = re.search(r'^set\(' + name + r' (.*)\)$', text, re.MULTILINE)
            self.assertIsNotNone(match, name)
            return re.findall(r'"([^"]*)"', match[1])
        lower = values('APP_CHUNK_LOWER')
        self.assertEqual(len(lower) % 3, 0)
        for source, output, how in zip(lower[0::3], lower[1::3], lower[2::3]):
            mode = ['--rom', how[4:]] if how.startswith('rom:') else []
            subprocess.run(['node', str(bc.ROOT / 'tools/kasane_ir/lower_plans.mjs'),
                            *mode, '--file', source, output], check=True)
        files = values('APP_CHUNK_FILES')
        self.assertTrue(files)
        for filename in files:
            source = Path(filename)
            out = generated / 'bytecode' / (source.name + '.bc')
            bc.generate(source, out, self.compiler, self.identity)
            manifest = bc.verify(source, out)
            self.assertEqual(manifest['source_sha256'], bc.digest(source))
            self.assertEqual(manifest['filename'], source.name)

    def test_read_execute_failalloc_cleanup(self):
        if FUNCTIONAL_ONLY:
            self.skipTest('--functional-only excludes allocator fault injection')
        source = self.root / 'failalloc.js'
        source.write_text('function f(x){return ()=>({answer:x+1});} let a=f(41); a().answer;')
        out = self.root / 'failalloc.bc'
        bc.generate(source, out, self.compiler, self.identity)
        statuses = set()
        for n in range(121):
            with self.subTest(fail_after=n):
                statuses.add(self.evaluate('bc', out, source.name, fail_after=n)[0])
        self.assertIn(1, statuses)
        self.assertIn(0, statuses)
        self.assertEqual(self.evaluate('bc', out, source.name)[0], 0)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc')
    parser.add_argument('--sanitize', action='store_true',
                        help='instrument the exact engine and both drivers with ASan/UBSan')
    parser.add_argument('--functional-only', action='store_true',
                        help='skip low-heap/OOM and allocator fault-injection tests')
    args, rest = parser.parse_known_args()
    CC = args.cc
    FUNCTIONAL_ONLY = args.functional_only
    if args.sanitize:
        if not CC:
            parser.error('--sanitize requires --cc')
        bc.FLAGS.extend(['-O1', '-g', '-fno-omit-frame-pointer',
                         '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined'])
        if sys.platform.startswith('linux'):
            # A fixed executable mapping avoids known GCC ASan startup-only
            # failures on container kernels; real sanitizer findings still fail.
            bc.FLAGS.extend(['-fno-pie', '-no-pie'])
    # The decorator runs on import, before command-line parsing.
    CompilerTests.__unittest_skip__ = not bool(CC)
    unittest.main(argv=['test_precompile_apps.py', *rest])
