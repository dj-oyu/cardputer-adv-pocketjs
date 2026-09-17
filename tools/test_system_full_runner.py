"""Fail-closed orchestration checks; subprocesses and serial are never real."""
import contextlib
import importlib.util
import io
import json
import os
import runpy
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('system_full_test', Path(__file__).with_name('system_full_test.py'))
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class RunnerTest(unittest.TestCase):
    def run_case(self, *, host=False, fail=None, cleanup=False):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            out, build = root / 'out', root / 'build'
            build.mkdir()
            (build / 'sdkconfig').write_text('CONFIG_KSN_DEVICE_PROBE=y\n')
            (build / 'cardputer_pocketjs.bin').write_bytes(b'audited firmware')
            commands = []

            def execute(command, **kwargs):
                command = list(map(str, command))
                commands.append(command)
                if 'tools/kasane_only_device_test.py' in command:
                    target = out / 'device-lifetime'
                    target.mkdir()
                    (target / 'memory.json').write_text('[[1000,500],[1000,500]]')
                class Result:
                    returncode = int(bool(fail and any(fail in arg for arg in command)))
                return Result()

            class Serial:
                def __init__(self, *args, **kwargs):
                    if cleanup:
                        raise OSError('cleanup unavailable')
                def __enter__(self): return self
                def __exit__(self, *args): pass
                def write(self, data): pass

            import types
            argv = ['runner', '--out', str(out), '--build', str(build), '--cycles', '2']
            argv += ['--host-only'] if host else ['--nm', 'fake-nm', '--port', 'FAKE', '--flash']
            with patch.object(runner, 'ROOT', root), patch.object(sys, 'argv', argv), \
                 patch.object(runner.subprocess, 'run', side_effect=execute), \
                 patch.object(runner.subprocess, 'check_output', return_value='test-head'), \
                 patch.dict(os.environ, {'IDF_PATH': str(root)}), \
                 patch.dict(sys.modules, {'serial': types.SimpleNamespace(Serial=Serial)}), \
                 patch.object(runner.time, 'sleep'), contextlib.redirect_stdout(io.StringIO()), \
                 contextlib.redirect_stderr(io.StringIO()):
                code = runner.main()
            return code, json.loads((out / 'report.json').read_text()), commands

    def test_host_is_partial(self):
        code, report, commands = self.run_case(host=True)
        self.assertEqual((code, report['status']), (0, 'HOST_PASS_DEVICE_NOT_RUN'))
        self.assertEqual(len(commands), 4)

    def test_host_failure_stops(self):
        code, report, commands = self.run_case(host=True, fail='build_power_test.sh')
        self.assertEqual((code, report['status'], len(commands)), (1, 'FAIL', 1))

    def test_audit_failure_never_flashes(self):
        code, report, commands = self.run_case(fail='check_kasane_link.py')
        self.assertEqual((code, report['status']), (1, 'FAIL'))
        self.assertFalse(any('esptool' in c for c in commands))

    def test_cleanup_failure_fails_exit(self):
        code, report, _ = self.run_case(cleanup=True)
        self.assertEqual((code, report['status']), (1, 'FAIL'))
        self.assertIn('cleanup_error', report)

    def test_full_success_records_artifact(self):
        code, report, _ = self.run_case()
        self.assertEqual((code, report['status']), (0, 'FULL_PASS'))
        self.assertEqual(len(report['firmware_sha256']), 64)


class LinkAuditTest(unittest.TestCase):
    def audit(self, component='quickjs-ng', source='', symbol=''):
        with tempfile.TemporaryDirectory() as folder:
            build = Path(folder)
            (build / 'project_description.json').write_text(json.dumps({'build_components': [component]}))
            for name in ('cardputer_pocketjs.map', 'build.ninja', 'compile_commands.json'):
                (build / name).write_text(source)
            symbols = 'ksn_core_init pocket_input_install pocketjs_guest_frame ' + symbol
            with patch.object(sys, 'argv', ['audit', '--build', str(build), '--nm', 'mock-nm']), \
                 patch.object(runner.subprocess, 'check_output', return_value=symbols), \
                 contextlib.redirect_stdout(io.StringIO()):
                runpy.run_path(str(Path(__file__).with_name('check_kasane_link.py')), run_name='__main__')

    def test_clean_graph(self): self.audit()

    def test_taffy_component_rejected(self):
        with self.assertRaises(AssertionError): self.audit(component='Taffy')

    def test_taffy_source_rejected(self):
        with self.assertRaises(AssertionError): self.audit(source='vendor/taffy/layout.rs')

    def test_taffy_symbol_rejected(self):
        with self.assertRaises(AssertionError): self.audit(symbol='taffy::layout')


if __name__ == '__main__':
    unittest.main()
