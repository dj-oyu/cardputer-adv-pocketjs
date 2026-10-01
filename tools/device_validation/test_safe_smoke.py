"""Offline tests: no serial package, device, ESP-IDF or downloads required."""
import importlib.util
import io
from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('safe_smoke', ROOT / 'safe_smoke.py')
smoke = importlib.util.module_from_spec(spec)
spec.loader.exec_module(smoke)


class Port:
    def __init__(self, wrong_identity=False, fault=False):
        self.keys = []
        self.pending = b''
        self.running = False
        self.wrong_identity, self.fault = wrong_identity, fault

    def write(self, key):
        self.keys.append(key)
        if key == b'q':
            self.pending = (b'APP_STOPPED\r\nMEM free=100000 largest=64000\r\n'
                            if self.running else b'') + b'HOME_READY\r\n'
            self.running = False
        elif key == b'a':
            self.pending = b'CATEGORY 0\r\n'
        elif key == b'u':
            self.pending = b'APP 0\r\n'
        elif key == b'e':
            if self.running:
                self.pending = b'HELLO_COUNT 1\r\n'
            else:
                self.running = True
                self.pending = (b'APP_ID local.wrong' if self.wrong_identity else b'APP_ID local.hello')
                self.pending += b'\r\nKASANE_FRAME_PRESENTED\r\n'
        if self.fault:
            self.pending = b'Guru Meditation\r\n' + self.pending

    def read(self, count):
        result = self.pending[:7]  # Exercise markers split across serial reads.
        self.pending = self.pending[7:]
        return result


class OfflineTests(unittest.TestCase):
    def test_normal_cycles_preserve_logs(self):
        port, log = Port(), io.BytesIO()
        result = smoke.run_smoke(port, log, 3)
        self.assertEqual(result['status'], 'passed')
        self.assertEqual(len(result['post_stop_memory']), 3)
        self.assertEqual(port.keys, [b'q', b'a'] + [b'u'] * 14 + [b'e', b'e', b'q'] * 3)
        self.assertIn(b'APP_STOPPED', log.getvalue())
        self.assertFalse(port.running)

    def test_ansi_colors_do_not_break_markers(self):
        class ColorPort(Port):
            def write(self, key):
                super().write(key)
                self.pending = b'\x1b[32m' + self.pending.replace(b'\r\n', b'\x1b[0m\r\n')
        self.assertEqual(smoke.run_smoke(ColorPort(), io.BytesIO(), 1)['status'], 'passed')

    def test_wrong_app_fails(self):
        with self.assertRaisesRegex(RuntimeError, 'identity'):
            smoke.run_smoke(Port(wrong_identity=True), io.BytesIO(), 1)

    def test_fault_keeps_partial_log(self):
        log = io.BytesIO()
        with self.assertRaisesRegex(RuntimeError, 'fault'):
            smoke.run_smoke(Port(fault=True), log, 1)
        self.assertIn(b'Guru Meditation', log.getvalue())

    def test_timeout_is_bounded(self):
        now = iter(range(100))
        with self.assertRaisesRegex(RuntimeError, 'Timeout'):
            smoke.Session(Port(), io.BytesIO(), clock=lambda: next(now)).command(b'q', b'NEVER', timeout=5)

    def test_diagnostic_key_rejected(self):
        with self.assertRaises(ValueError):
            smoke.Session(Port(), io.BytesIO()).command(b'1', b'PASS')

    def test_grid_equal_all_modes(self):
        raw = b''.join(b'GRID_APP ' + kind + b' ' + str(mode).encode() + b' scalar_us=2 equal=1\n'
                       for kind in (b'MEASURE', b'ROUTES') for mode in (3, 4, 5))
        self.assertEqual(smoke.check_grid_log(raw)['grid_measure_and_route_equality'], 'passed')
        with self.assertRaises(RuntimeError):
            smoke.check_grid_log(raw.replace(b'equal=1', b'equal=0', 1))
        with self.assertRaises(RuntimeError):
            smoke.check_grid_log(raw.replace(b'ROUTES 5', b'ROUTES 6'))
        with self.assertRaises(RuntimeError):
            smoke.check_grid_log(raw + b'Guru Meditation')

    def test_cli_no_opt_in_fails_without_importing_serial(self):
        proc = subprocess.run([sys.executable, str(ROOT / 'safe_smoke.py')], capture_output=True, text=True)
        self.assertEqual(proc.returncode, 2)
        self.assertIn('no default port', proc.stderr)
        self.assertNotIn('ModuleNotFoundError', proc.stderr)

    def test_powershell_guardrails(self):
        build = (ROOT / 'Build-Validation.ps1').read_text()
        flash = (ROOT / 'Flash-Validation.ps1').read_text()
        device = (ROOT / 'Test-Device.ps1').read_text()
        self.assertIn("'worktree','add','--detach'", build)
        self.assertIn('89a71ae72c721206dfbb63acfeffc74b38dde604', build)
        self.assertIn('-DSDKCONFIG=', build)
        self.assertNotIn('COM3', build + flash + device)
        self.assertNotIn('erase-flash', flash)
        self.assertIn("'--before','no-reset','--after','no-reset'", flash)
        self.assertLess(flash.index('if (!$AllowFlash'), flash.index('Get-IdfPython'))
        self.assertLess(device.index('if (!$AllowDevice'), device.index('Get-IdfPython'))
        self.assertNotIn('smoke_device.py', device)
        self.assertNotIn('stress_app.py', device)


if __name__ == '__main__':
    unittest.main()
