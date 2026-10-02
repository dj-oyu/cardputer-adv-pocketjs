"""Mock-only integrated-runner tests; no serial package or device is needed."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location('integrated_smoke', ROOT / 'integrated_smoke.py')
smoke = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(smoke)


class Clock:
    def __init__(self, step=0.002):
        self.now, self.step = 0, step

    def __call__(self):
        self.now += self.step
        return self.now


def grid_log():
    lines = []
    for mode in (3, 4, 5):
        lines.extend((f'GRID_APP MEASURE {mode} repeats=8 scalar_us=160 pie_us=80 equal=1',
                      f'GRID_APP ROUTES {mode} gather_us=70 affine_us=80 equal=1',
                      f'GRID_APP MODE {mode} 48x28 backend=PIE strategy=AFFINE'))
    for arm, (layout, route) in enumerate(smoke.EXPECTED_ARMS):
        backend = 'PIE' if route == 'AUTO' else 'scalar'
        lines.extend((f'GRID_APP ARM {arm} layout={layout} route={route}',
                      f'GRID_APP MODE {arm + 6} 48x28 backend={backend} strategy=AFFINE'))
        lines += ['KASANE_PAINT turn_ms=12.00 render_ms=2.00 send_ms=8.00 bytes=64800'] * 6
        lines.append(f'GRID_APP PIPE arm={arm} layout={layout} route={route} frames=180 '
                     'source_us=300 run_us=600 native_copy_us=10 bind_us=20 kernel_us=100 '
                     'native_total_us=150 native_runs=180')
        lines.append(f'GRID_APP MEM arm={arm} native_max_us=9 heap_free=100000 '
                     'heap_largest=64000 heap_min_free=80000 max_gap_us=33000 '
                     'gaps_over_50ms=0 gaps_over_75ms=0')
    return ('\r\n'.join(lines) + '\r\n').encode()


class Port:
    def __init__(self, chunk=32768, transform=None, colored=False):
        self.keys = []
        self.pending = b''
        self.row = 13
        self.running = None
        self.chunk, self.transform, self.colored = chunk, transform, colored
        self.closed = False
        self.open_state = None
        self.dtr = self.rts = True
        self.port = None

    def __enter__(self):
        self.open_state = (self.port, self.dtr, self.rts)
        return self

    def __exit__(self, *unused):
        self.closed = True

    def write(self, key):
        self.keys.append(key)
        if key == b'q':
            data = (b'MEM free=100000 largest=64000 js=0 frames=200\r\nAPP_STOPPED\r\n'
                    if self.running else b'') + b'HOME_READY\r\n'
            self.running = None
        elif key == b'a':
            data = b'CATEGORY 0\r\n'
        elif key in (b'u', b'd'):
            self.row = max(0, self.row - 1) if key == b'u' else min(13, self.row + 1)
            data = b'APP ' + str(self.row).encode() + b'\r\n'
        elif key == b'e' and self.running == 'hello':
            data = b'HELLO_COUNT 1\r\n'
        elif key == b'e' and self.row == 0:
            self.running = 'hello'
            data = (b'APP_ID local.hello\r\nHELLO_READY\r\n'
                    b'MEM free=50000 largest=20000 js=70000 frames=0\r\n'
                    b'KASANE_FRAME_PRESENTED\r\n')
        elif key == b'e' and self.row == 9:
            self.running = 'grid'
            # The automatic run intentionally arrives in the same read as startup.
            data = (b'APP_ID local.gridlab\r\nGRID_APP MODE 0 28x14 backend=PIE strategy=RESIZE\r\n'
                    + grid_log() + b'GRID_APP MODE 0 28x14 backend=PIE strategy=RESIZE\r\n')
        else:
            raise AssertionError('Unexpected key or app launch: ' + repr((key, self.row)))
        if self.transform:
            data = self.transform(key, data)
        if self.colored:
            data = b''.join(b'\x1b[32mI (123) js: ' + line + b'\x1b[0m\r\n'
                            for line in data.split(b'\r\n') if line)
        self.pending += data
        return len(key)

    def read(self, count):
        data = self.pending[:min(count, self.chunk)]
        self.pending = self.pending[len(data):]
        return data


def run(port=None, cycles=1, grid=False, result=None, clock=None):
    port = Port() if port is None else port
    log = io.BytesIO()
    result = smoke.run_smoke(port, log, cycles, grid=grid,
                             clock=clock or Clock(), result=result)
    return result, log.getvalue(), port


class SessionTests(unittest.TestCase):
    def test_complete_hello_cycles_and_no_extra_keys(self):
        result, raw, port = run(cycles=3)
        self.assertEqual(result['status'], 'passed')
        self.assertEqual(result['hello_completed'], 3)
        self.assertEqual(port.keys, [b'q', b'a'] + [b'u'] * 14 + [b'e', b'e', b'q'] * 3)
        self.assertIsNone(port.running)
        self.assertEqual(result['post_stop_memory'], [dict(free=100000, largest=64000, js=0,
                          frames=200, line='MEM free=100000 largest=64000 js=0 frames=200')] * 3)
        self.assertEqual(raw.count(b'APP_STOPPED'), 3)
        self.assertEqual(result['performance_claim'], 'none')
        self.assertEqual(result['physical_checks'], 'not assessed')

    def test_maximum_cycles(self):
        self.assertEqual(run(cycles=20)[0]['hello_completed'], 20)

    def test_grid_fragmented_coalesced_and_colored(self):
        for chunk, colored in ((1, False), (7, False), (32768, False), (1, True), (32768, True)):
            with self.subTest(chunk=chunk, colored=colored):
                result, raw, port = run(Port(chunk=chunk, colored=colored), grid=True)
                self.assertEqual(result['grid']['status'], 'passed')
                self.assertEqual(result['grid']['equality_modes'], [3, 4, 5])
                self.assertEqual(len(result['grid']['arms']), 4)
                self.assertEqual(result['grid']['arms'][3]['frames'], 180)
                self.assertTrue(result['grid']['arms'][3]['memory_line'].endswith('gaps_over_75ms=0'))
                self.assertEqual(port.keys, [b'q', b'a'] + [b'u'] * 14 + [b'e', b'e', b'q'] +
                                 [b'd'] * 9 + [b'e', b'q'])
                self.assertEqual(raw.count(b'APP_ID'), 2)
                self.assertIsNone(port.running)

    def test_pending_suffix_survives_next_wait(self):
        port = Port()
        port.pending = b'FIRST\r\nSECOND\r\nTHIRD'
        log = io.BytesIO()
        session = smoke.Session(port, log, Clock())
        self.assertEqual(session.collect(rb'FIRST'), b'FIRST\n')
        self.assertEqual(session.collect(rb'SECOND'), b'SECOND\n')
        port.pending += b'\r\n'
        self.assertEqual(session.collect(rb'THIRD'), b'THIRD\n')
        self.assertEqual(log.getvalue(), b'FIRST\r\nSECOND\r\nTHIRD\r\n')

    def test_old_coalesced_ack_cannot_satisfy_new_command(self):
        port = Port(transform=lambda key, data: b'OTHER\r\n')
        port.pending = b'FIRST\nHOME_READY\n'
        session = smoke.Session(port, io.BytesIO(), Clock(step=0.5))
        session.collect(rb'FIRST')
        with self.assertRaisesRegex(RuntimeError, 'Timeout'):
            session.command(b'q', rb'HOME_READY', timeout=2)

    def test_incomplete_old_line_cannot_satisfy_new_command(self):
        port = Port(transform=lambda key, data: b'_READY\n')
        port.pending = b'FIRST\nHOME'
        session = smoke.Session(port, io.BytesIO(), Clock(step=0.5))
        session.collect(rb'FIRST')
        with self.assertRaisesRegex(RuntimeError, 'Timeout'):
            session.command(b'q', rb'HOME_READY', timeout=2)

    def test_no_diagnostic_reset_upload_or_compound_keys(self):
        port = Port()
        session = smoke.Session(port, io.BytesIO(), Clock())
        for key in (b'1', b'K', b'Q', b'~', b'?', b'b', b'', b'ee', b'\r', b'\x1b'):
            with self.subTest(key=key), self.assertRaises(ValueError):
                session.command(key, rb'anything')
        self.assertEqual(port.keys, [])

    def test_timeout_is_bounded(self):
        clock = Clock(step=0.5)
        with self.assertRaisesRegex(RuntimeError, 'Timeout'):
            smoke.Session(Port(), io.BytesIO(), clock).collect(rb'NEVER', timeout=3)
        self.assertLess(clock.now, 6)

    def test_overall_deadline_prevents_key(self):
        port, clock = Port(), Clock()
        session = smoke.Session(port, io.BytesIO(), clock)
        clock.now = 541
        with self.assertRaisesRegex(RuntimeError, 'Overall'):
            session.command(b'q', rb'HOME_READY')
        self.assertEqual(port.keys, [])

    def test_short_write_fails(self):
        port = Port()
        port.write = lambda key: 0
        with self.assertRaisesRegex(RuntimeError, 'Incomplete serial key write'):
            smoke.Session(port, io.BytesIO(), Clock()).command(b'q', rb'HOME_READY')

    def test_fault_in_same_read_after_terminal_marker_is_not_hidden(self):
        port = Port(transform=lambda key, data: data + b'Guru Meditation\r\n')
        log = io.BytesIO()
        with self.assertRaisesRegex(RuntimeError, 'fault'):
            smoke.run_smoke(port, log, 1, clock=Clock())
        self.assertIn(b'HOME_READY\r\nGuru Meditation', log.getvalue())
        self.assertEqual(port.keys, [b'q'])

    def test_fragmented_ansi_fault_is_logged_without_recovery(self):
        port = Port(chunk=1, transform=lambda key, data: b'Guru \x1b[31mMeditation\x1b[0m')
        log = io.BytesIO()
        with self.assertRaisesRegex(RuntimeError, 'fault'):
            smoke.run_smoke(port, log, 1, clock=Clock())
        self.assertIn(b'Guru \x1b[31mMeditation', log.getvalue())
        self.assertEqual(port.keys, [b'q'])

    def test_unterminated_marker_times_out(self):
        port = Port(transform=lambda key, data: b'HOME_READY')
        with self.assertRaisesRegex(RuntimeError, 'Timeout'):
            run(port, clock=Clock(step=0.5))

    def test_trailing_fault_and_partial_line_fail(self):
        for suffix in (b"abort()\n", b'UNFINISHED'):
            with self.subTest(suffix=suffix):
                port = Port(transform=lambda key, data: data + suffix if b'APP_STOPPED' in data else data)
                with self.assertRaises(RuntimeError):
                    run(port)

    def test_identity_missing_suffix_duplicate_and_wrong(self):
        for replacement in (b'', b'APP_ID local.hellox\r\n', b'APP_ID local.wrong\r\n',
                            b'APP_ID local.hello\r\nAPP_ID local.hello\r\n'):
            with self.subTest(replacement=replacement):
                port = Port(transform=lambda key, data: data.replace(b'APP_ID local.hello\r\n', replacement))
                with self.assertRaisesRegex(RuntimeError, 'identity'):
                    run(port)
                self.assertEqual(port.keys[-1], b'e')

    def test_identity_after_presented_in_same_read_is_rejected(self):
        port = Port(transform=lambda key, data: data.replace(
            b'KASANE_FRAME_PRESENTED\r\n',
            b'KASANE_FRAME_PRESENTED\r\nAPP_ID local.wrong\r\n'))
        with self.assertRaisesRegex(RuntimeError, 'identity'):
            run(port)
        self.assertEqual(port.keys[-1], b'e')

    def test_identity_late_in_run_is_rejected(self):
        port = Port(transform=lambda key, data: data + b'APP_ID local.wrong\r\n'
                    if b'APP_STOPPED' in data else data)
        with self.assertRaisesRegex(RuntimeError, 'identity'):
            run(port)

    def test_grid_identity_wrong_does_not_send_exit_key(self):
        port = Port(transform=lambda key, data: data.replace(b'local.gridlab', b'local.hello'))
        with self.assertRaisesRegex(RuntimeError, 'identity'):
            run(port, grid=True)
        self.assertEqual(port.keys[-1], b'e')

    def test_hello_ready_required(self):
        port = Port(transform=lambda key, data: data.replace(b'HELLO_READY\r\n', b''))
        with self.assertRaisesRegex(RuntimeError, 'HELLO_READY'):
            run(port)

    def test_navigation_checks_category_and_row(self):
        for old, new in ((b'CATEGORY 0', b'CATEGORY 1'), (b'APP 12', b'APP 99'),
                         (b'APP 5', b'APP 6')):
            with self.subTest(new=new), self.assertRaises(RuntimeError):
                run(Port(transform=lambda key, data: data.replace(old, new)), grid=True)

    def test_bad_grid_row_fails_without_launch(self):
        port = Port(transform=lambda key, data: b'APP 8\r\n' if key == b'd' and data == b'APP 9\r\n' else data)
        with self.assertRaisesRegex(RuntimeError, 'row 9'):
            run(port, grid=True)
        self.assertEqual(port.keys[-1], b'd')

    def test_bad_grid_does_not_send_cleanup(self):
        port = Port(transform=lambda key, data: data.replace(b'equal=1', b'equal=0', 1))
        with self.assertRaisesRegex(RuntimeError, 'unequal'):
            run(port, grid=True)
        self.assertEqual(port.keys[-1], b'e')
        self.assertEqual(port.running, 'grid')

    def test_truncated_final_grid_mem_never_counts_as_completion(self):
        port = Port(transform=lambda key, data: data[:data.index(b' gaps_over_75ms=', data.index(b'GRID_APP MEM arm=3'))]
                    if b'GRID_APP MEM arm=3' in data else data)
        with self.assertRaisesRegex(RuntimeError, 'Timeout'):
            run(port, grid=True, clock=Clock(step=0.25))
        self.assertEqual(port.keys[-1], b'e')

    def test_cycles_rejected_before_any_key(self):
        for cycles in (0, 21, -1, True, 1.5, '1'):
            port = Port()
            with self.subTest(cycles=cycles), self.assertRaises(ValueError):
                run(port, cycles=cycles)
            self.assertEqual(port.keys, [])


class ParserTests(unittest.TestCase):
    def test_grid_all_fields_and_labels_are_preserved(self):
        result = smoke.check_grid_log(grid_log())
        self.assertEqual([(arm['layout'], arm['route']) for arm in result['arms']], list(smoke.EXPECTED_ARMS))
        self.assertEqual(result['arms'][0]['native_runs'], 180)
        self.assertEqual(result['arms'][3]['gaps_over_75ms'], 0)
        self.assertEqual(result['performance_claim'], 'none')

    def test_prefixed_ansi_grid_log(self):
        raw = b''.join(b'\x1b[32mI (0001) js: ' + line + b'\x1b[0m\n' for line in grid_log().splitlines())
        self.assertEqual(smoke.check_grid_log(raw)['status'], 'passed')

    def test_malformed_and_incomplete_grid_logs_fail(self):
        mutations = [
            lambda raw: raw.replace(b'GRID_APP MEM arm=3', b'OTHER MEM arm=3'),
            lambda raw: raw.rstrip(b'\r\n'),
            lambda raw: raw.replace(b'equal=1', b'equal=0', 1),
            lambda raw: raw.replace(b'equal=1', b'equal=10', 1),
            lambda raw: raw.replace(b'repeats=8', b'repeats=7', 1),
            lambda raw: raw.replace(b'GRID_APP ROUTES 5', b'OTHER ROUTES 5'),
            lambda raw: raw.replace(b'GRID_APP MEASURE 3', b'GRID_APP MEASURE 2'),
            lambda raw: raw.replace(b'frames=180', b'frames=179', 1),
            lambda raw: raw.replace(b'native_runs=180', b'native_runs=179', 1),
            lambda raw: raw.replace(b'layout=FULL route=AUTO', b'layout=WINDOW route=AUTO', 1),
            lambda raw: raw.replace(b'layout=FULL route=SCALAR frames', b'layout=FULL route=AUTO frames', 1),
            lambda raw: raw.replace(b'MODE 6 48x28 backend=PIE', b'MODE 6 48x28 backend=scalar'),
            lambda raw: raw.replace(b'MODE 9', b'MODE 8'),
            lambda raw: raw.replace(b'MODE 9', b'OTHER 9'),
            lambda raw: raw.replace(b'heap_free=100000', b'heap_free=0', 1),
            lambda raw: raw.replace(b'heap_largest=64000', b'heap_largest=100001', 1),
            lambda raw: raw.replace(b'heap_min_free=80000', b'heap_min_free=0', 1),
            lambda raw: raw.replace(b'native_total_us=150', b'native_total_us=129', 1),
            lambda raw: raw.replace(b'run_us=600', b'run_us=149', 1),
            lambda raw: raw.replace(b'gaps_over_75ms=0', b'gaps_over_75ms=', 1),
            lambda raw: raw + raw,
            lambda raw: raw + b"panic'ed\n",
        ]
        for number, mutate in enumerate(mutations):
            with self.subTest(number=number), self.assertRaises(RuntimeError):
                smoke.check_grid_log(mutate(grid_log()))

    def test_pipeline_and_mode_ordering(self):
        lines = grid_log().splitlines(keepends=True)
        for first, second in ((b'GRID_APP ARM 0', b'GRID_APP PIPE arm=0'),
                              (b'GRID_APP MODE 6', b'GRID_APP MEM arm=0'),
                              (b'GRID_APP PIPE arm=1', b'GRID_APP MEM arm=1')):
            modified = lines[:]
            a = next(i for i, line in enumerate(lines) if line.startswith(first))
            b = next(i for i, line in enumerate(lines) if line.startswith(second))
            modified[a], modified[b] = modified[b], modified[a]
            with self.subTest(first=first), self.assertRaises(RuntimeError):
                smoke.check_grid_log(b''.join(modified))

    def test_equality_must_precede_pipeline(self):
        lines = grid_log().splitlines(keepends=True)
        lines.append(lines.pop(0))
        with self.assertRaisesRegex(RuntimeError, 'precede'):
            smoke.check_grid_log(b''.join(lines))

    def test_complete_final_memory_required(self):
        valid = b'MEM free=100 largest=80 js=0 frames=10\r\nAPP_STOPPED\r\nHOME_READY\r\n'
        self.assertEqual(smoke.final_memory(valid)['frames'], 10)
        malformed = [valid.replace(b' js=0 frames=10', b''),
                     valid.replace(b' js=0', b' js=2'),
                     valid.replace(b'frames=10', b'frames=0'),
                     valid.replace(b'largest=80', b'largest=101'),
                     valid.replace(b'APP_STOPPED\r\n', b''),
                     valid.replace(b'APP_STOPPED', b'APP_SUSPENDED'),
                     valid.replace(b'HOME_READY\r\n', b'HOME_READY'),
                     valid + valid,
                     b'APP_STOPPED\r\n' + valid.replace(b'APP_STOPPED\r\n', b'')]
        for number, raw in enumerate(malformed):
            with self.subTest(number=number), self.assertRaises(RuntimeError):
                smoke.final_memory(raw)

    def test_fault_markers(self):
        for fault in (b'Guru Meditation', b"panic'ed", b'START_FAILED', b'PRESENTER_STEP_FAILED',
                      b'APP_ERROR', b'APP_REFUSED', b'abort()', b'ESP_ERROR_CHECK failed',
                      b'Brownout detector', b'Task watchdog', b'FRAME RUNAWAY'):
            with self.subTest(fault=fault), self.assertRaisesRegex(RuntimeError, 'fault'):
                smoke.check_grid_log(grid_log() + fault)


class CliTests(unittest.TestCase):
    def test_default_cli_refuses_without_importing_serial(self):
        process = subprocess.run([sys.executable, str(ROOT / 'integrated_smoke.py')], capture_output=True, text=True)
        self.assertEqual(process.returncode, 2)
        self.assertIn('no default port', process.stderr)
        self.assertNotIn('ModuleNotFoundError', process.stderr)

    def test_module_import_does_not_import_serial(self):
        program = ('import runpy, sys; runpy.run_path(sys.argv[1]); '
                   'assert "serial" not in sys.modules')
        process = subprocess.run([sys.executable, '-c', program, str(ROOT / 'integrated_smoke.py')],
                                 capture_output=True, text=True)
        self.assertEqual(process.returncode, 0, process.stderr)

    def test_all_opt_ins_and_cycles_checked_before_factory_or_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory) / 'output'
            base = ['--allow-device', '--port', 'MOCK', '--out', str(out)]
            invalid = [[], ['--grid'], base[1:], base[:1] + base[3:], base[:3],
                       base + ['--cycles', '0'], base + ['--cycles', '21'],
                       ['--allow-device', '--port', ' ', '--out', str(out)]]
            factory = mock.Mock(side_effect=AssertionError('Factory must not be reached'))
            for args in invalid:
                with self.subTest(args=args), contextlib.redirect_stderr(io.StringIO()):
                    with self.assertRaises(SystemExit) as error:
                        smoke.main(args, serial_factory=factory)
                    self.assertEqual(error.exception.code, 2)
                    self.assertFalse(out.exists())
            factory.assert_not_called()

    def invoke(self, directory, port, extra=()):
        factory = mock.Mock(return_value=port)
        original_run = smoke.run_smoke
        def fast_run(*args, **kwargs):
            return original_run(*args, **kwargs, clock=Clock())
        with mock.patch.object(smoke, 'run_smoke', side_effect=fast_run), contextlib.redirect_stdout(io.StringIO()):
            code = smoke.main(['--allow-device', '--port', 'MOCK', '--out', str(directory),
                               '--cycles', '1', *extra], serial_factory=factory)
        return code, factory

    def test_success_uses_no_reset_port_configuration_and_saves_logs(self):
        with tempfile.TemporaryDirectory() as directory:
            out, port = Path(directory) / 'run', Port()
            code, factory = self.invoke(out, port, extra=['--grid'])
            self.assertEqual(code, 0)
            factory.assert_called_once_with(port=None, baudrate=115200, timeout=0.1, write_timeout=2)
            self.assertEqual(port.open_state, ('MOCK', False, False))
            self.assertTrue(port.closed)
            result = json.loads((out / 'result.json').read_text())
            self.assertEqual(result['status'], 'passed')
            self.assertNotIn('error', result)
            self.assertIn(b'GRID_APP MEM arm=3', (out / 'serial.log').read_bytes())

    def test_failure_saves_partial_observations_and_raw_fault(self):
        def transform(key, data):
            return data.replace(b'APP_ID local.gridlab', b'Guru Meditation')
        with tempfile.TemporaryDirectory() as directory:
            out, port = Path(directory) / 'run', Port(transform=transform)
            code, _ = self.invoke(out, port, extra=['--grid'])
            self.assertEqual(code, 1)
            self.assertTrue(port.closed)
            result = json.loads((out / 'result.json').read_text())
            self.assertEqual(result['status'], 'failed')
            self.assertEqual(result['hello_completed'], 1)
            self.assertEqual(len(result['post_stop_memory']), 1)
            self.assertEqual(result['phase'], 'grid-pipeline')
            self.assertIn(b'Guru Meditation', (out / 'serial.log').read_bytes())
            self.assertEqual(port.keys[-1], b'e')

    def test_open_failure_still_writes_result_and_empty_log(self):
        class CannotOpen(Port):
            def __enter__(self):
                raise OSError('Mock port unavailable')
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory) / 'run'
            code, _ = self.invoke(out, CannotOpen())
            self.assertEqual(code, 1)
            self.assertEqual((out / 'serial.log').read_bytes(), b'')
            self.assertIn('Mock port unavailable', json.loads((out / 'result.json').read_text())['error'])

    def test_missing_serial_package_preserves_failure_result(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory) / 'run'
            with mock.patch.dict(sys.modules, {'serial': None}), contextlib.redirect_stdout(io.StringIO()):
                code = smoke.main(['--allow-device', '--port', 'MOCK', '--out', str(out)])
            self.assertEqual(code, 1)
            self.assertEqual((out / 'serial.log').read_bytes(), b'')
            self.assertEqual(json.loads((out / 'result.json').read_text())['status'], 'failed')

    def test_keyboard_interrupt_preserves_partial_log(self):
        class Interrupted(Port):
            def read(self, count):
                if len(self.keys) > 1:
                    raise KeyboardInterrupt()
                return super().read(count)
        with tempfile.TemporaryDirectory() as directory:
            out, port = Path(directory) / 'run', Interrupted()
            code, _ = self.invoke(out, port)
            self.assertEqual(code, 130)
            self.assertTrue(port.closed)
            self.assertIn(b'HOME_READY', (out / 'serial.log').read_bytes())
            self.assertEqual(json.loads((out / 'result.json').read_text())['status'], 'failed')

    def test_existing_output_is_not_overwritten(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            result = out / 'result.json'
            result.write_text('preserve me')
            with self.assertRaises(FileExistsError):
                self.invoke(out, Port())
            self.assertEqual(result.read_text(), 'preserve me')


if __name__ == '__main__':
    unittest.main()
