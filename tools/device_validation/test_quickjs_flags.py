"""Offline flag/response-file checks; no toolchain, firmware, or device access."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import verify_quickjs_flags as guard


class QuickJsFlags(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='quickjs flags ')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()

    def entry(self, flags=None, command=None):
        item = {'file': r'C:\source\quickjs-ng\quickjs.c', 'directory': str(self.root)}
        if command is not None:
            item['command'] = command
        else:
            item['arguments'] = ['xtensa-esp32s3-elf-gcc.exe'] + (flags or [])
        return [item]

    def rsp(self, name, text):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding='utf-8')
        return path

    def test_actual_idf6_shape(self):
        # Same response-file contents and command shape as the failing Windows run.
        cflags = self.rsp('toolchain/cflags', '\n'.join([
            '-DPOCKET_VM_TYPED_PUT_INT_FAST=0', '-mlongcalls',
            '-fno-builtin-memcpy', '-fno-builtin-memset', '-fno-builtin-bzero',
            '"-specs=C:/devs/m5stack/pocketjs-validation/integrated-001/'
            'wt-integrated/build_integrated/specs/picolibc.specs"']))
        command = ('C:\\Espressif\\tools\\xtensa-esp-elf\\bin\\xtensa-esp32s3-elf-gcc.exe '
                   '-DESP_PLATFORM -DIDF_VER=\\"v6.0.1\\" -DQUICKJS_NG_BUILD '
                   f'@"{cflags.as_posix()}" -fdiagnostics-color=always -c '
                   'C:/devs/m5stack/source/components/quickjs-ng/quickjs-ng/quickjs.c')
        report = guard.verify(self.entry(command=command), 0)
        self.assertEqual(report['value'], 0)
        self.assertEqual(report['responseFiles'][0]['path'], str(cflags))
        self.assertEqual(len(report['responseFiles'][0]['sha256']), 64)

    def test_inline_and_split_and_quoted(self):
        for expected in (0, 1):
            for flags in ([f'-D{guard.MACRO}={expected}'], ['-D', f'{guard.MACRO}={expected}']):
                with self.subTest(flags=flags):
                    self.assertEqual(guard.verify(self.entry(flags), expected)['value'], expected)
        for argument in (f'"-D{guard.MACRO}=0"', f'-D{guard.MACRO}="0"'):
            self.assertEqual(guard.verify(self.entry(command='gcc ' + argument), 0)['value'], 0)

    def test_windows_backslashes_quotes_and_empty_argument(self):
        command = r'"C:\Program Files\gcc.exe" -IC:\sdk\include @"C:\build dir\toolchain\cflags" -DNAME=\"value\" ""'
        self.assertEqual(guard.split_windows_command(command), [
            r'C:\Program Files\gcc.exe', r'-IC:\sdk\include',
            r'@C:\build dir\toolchain\cflags', '-DNAME="value"', ''])
        self.assertEqual(guard.split_windows_command(r'"a\\" "x""y"'), ['a\\', 'x"y'])

    def test_response_gcc_quoting(self):
        self.assertEqual(guard.split_response(r'''"-IC:/some dir" '-DNAME=two words' -IC:\\sdk\\include a\ b'''),
                         ['-IC:/some dir', '-DNAME=two words', r'-IC:\sdk\include', 'a b'])

    def test_nested_relative_paths_are_compiler_cwd_relative(self):
        self.rsp('toolchain/outer', '@"nested flags"')
        self.rsp('nested flags', f'-D {guard.MACRO}=0')
        report = guard.verify(self.entry(['@toolchain/outer']), 0)
        self.assertEqual(len(report['responseFiles']), 2)

    def test_arguments_preferred_over_command(self):
        database = self.entry([f'-D{guard.MACRO}=0'])
        database[0]['command'] = f'gcc -D{guard.MACRO}=1'
        self.assertEqual(guard.verify(database, 0)['value'], 0)

    def test_missing_wrong_duplicate_or_undefined_is_rejected(self):
        good = f'-D{guard.MACRO}=0'
        cases = [[], [f'-D{guard.MACRO}=1'], [f'-D{guard.MACRO}'],
                 [f'-D{guard.MACRO}='], [good, good], [good, f'-D{guard.MACRO}=1'],
                 [good, '-U' + guard.MACRO], ['-U', guard.MACRO, good],
                 [good, '-Wp,-D,' + guard.MACRO + '=1'], [good, '-Xpreprocessor'],
                 [good, '-D' + guard.MACRO + ' =1'],
                 [good, '-U' + guard.MACRO + ' '],
                 [good, '-D ' + guard.MACRO + '=1'],
                 [f'-D{guard.MACRO}(x)=0'], [f'-D{guard.MACRO}_OTHER=0']]
        for flags in cases:
            with self.subTest(flags=flags), self.assertRaises(guard.VerificationError):
                guard.verify(self.entry(flags), 0)

    def test_response_conflict_and_undefine_are_rejected(self):
        for response in (f'-D{guard.MACRO}=1', f'-U{guard.MACRO}'):
            self.rsp('flags', response)
            with self.assertRaises(guard.VerificationError):
                guard.verify(self.entry([f'-D{guard.MACRO}=0', '@flags']), 0)

    def test_text_inside_another_definition_is_not_evidence(self):
        command = f'gcc -DSTRING="prefix -D{guard.MACRO}=0 suffix"'
        with self.assertRaises(guard.VerificationError):
            guard.verify(self.entry(command=command), 0)

    def test_missing_unreadable_and_cycle_fail_closed(self):
        with self.assertRaisesRegex(guard.VerificationError, 'Cannot read'):
            guard.verify(self.entry(['@missing']), 0)
        self.rsp('flags', f'-D{guard.MACRO}=0')
        with patch.object(Path, 'open', side_effect=PermissionError('denied')):
            with self.assertRaisesRegex(guard.VerificationError, 'Cannot read'):
                guard.verify(self.entry(['@flags']), 0)
        self.rsp('a', '@b')
        self.rsp('b', '@a')
        with self.assertRaisesRegex(guard.VerificationError, 'Cyclic'):
            guard.verify(self.entry(['@a']), 0)

    def test_expansion_limits(self):
        self.rsp('outer', '@inner')
        self.rsp('inner', f'-D{guard.MACRO}=0')
        for limit, value, flags, message in [
            ('MAX_DEPTH', 1, ['@outer'], 'depth'),
            ('MAX_FILES', 1, ['@outer'], 'file limit'),
            ('MAX_BYTES', 4, ['@outer'], 'byte'),
            ('MAX_TOKENS', 1, [f'-D{guard.MACRO}=0', '-O2'], 'token')]:
            with self.subTest(limit=limit), patch.object(guard, limit, value):
                with self.assertRaisesRegex(guard.VerificationError, message):
                    guard.verify(self.entry(flags), 0)

    def test_malformed_tokens_and_database(self):
        for tokenizer, value in [(guard.split_windows_command, 'gcc "unterminated'),
                                 (guard.split_response, "'unterminated"),
                                 (guard.split_response, 'trailing\\'),
                                 (guard.split_response, 'x\0y')]:
            with self.subTest(value=value), self.assertRaises(guard.VerificationError):
                tokenizer(value)
        for database in ([], self.entry() * 2, [{'file': 'quickjs.c', 'directory': 'relative'}]):
            with self.assertRaises(guard.VerificationError):
                guard.verify(database, 0)

    def test_cli_success_and_failure(self):
        database = self.root / 'compile_commands.json'
        helper = Path(guard.__file__)
        database.write_text(json.dumps(self.entry([f'-D{guard.MACRO}=0'])), encoding='utf-8')
        command = [sys.executable, str(helper), '--compile-commands', str(database), '--expected']
        result = subprocess.run(command + ['0'], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)['value'], 0)
        result = subprocess.run(command + ['1'], capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn('H=1 was not verified', result.stderr)


if __name__ == '__main__':
    unittest.main()
