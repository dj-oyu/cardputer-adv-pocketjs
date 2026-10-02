"""Opt-in, bounded checks of the embedded HELLO and optional GRID LAB apps.

This runner sends only ordinary menu/input keys. The PowerShell entry point
owns the shared serial/run locks and verifies the pinned image manifest. This
module never flashes, uploads JavaScript, resets, or attempts fault recovery.
"""
import argparse
import json
from pathlib import Path
import re
import time


ANSI = re.compile(rb'\x1b\[[0-?]*[ -/]*[@-~]')
PREFIX = re.compile(rb'^[IWEVD] \([^\r\n)]*\) [A-Za-z0-9_.-]+: ')
FAULT = re.compile(
    rb"Guru Meditation|panic(?:'ed)?|START_FAILED|PRESENTER_STEP_FAILED|"
    rb"APP_ERROR|APP_REFUSED|APP_LEGACY_UI|EXECUTION FAILED|abort\(\)|"
    rb"assert(?:ion)? failed|Stack canary|stack overflow|watchdog|"
    rb"Brownout detector|ESP_ERROR_CHECK failed|out of memory|"
    rb"FRAME RUNAWAY|JOB QUEUE RUNAWAY", re.IGNORECASE)
APP_ROWS = 14
GRID_ROW = 9
SAFE_KEYS = frozenset((b'q', b'a', b'u', b'd', b'e'))
EXPECTED_ARMS = (('WINDOW', 'AUTO'), ('WINDOW', 'SCALAR'),
                 ('FULL', 'AUTO'), ('FULL', 'SCALAR'))
ARM = re.compile(rb'GRID_APP ARM ([0-3]) layout=(WINDOW|FULL) route=(AUTO|SCALAR)')
PIPE = re.compile(
    rb'GRID_APP PIPE arm=([0-3]) layout=(WINDOW|FULL) route=(AUTO|SCALAR) '
    rb'frames=(\d+) source_us=(\d+) run_us=(\d+) native_copy_us=(\d+) '
    rb'bind_us=(\d+) kernel_us=(\d+) native_total_us=(\d+) native_runs=(\d+)')
MEM = re.compile(
    rb'GRID_APP MEM arm=([0-3]) native_max_us=(\d+) heap_free=(\d+) '
    rb'heap_largest=(\d+) heap_min_free=(\d+) max_gap_us=(\d+) '
    rb'gaps_over_50ms=(\d+) gaps_over_75ms=(\d+)')
MODE = re.compile(rb'GRID_APP MODE ([6-9]) 48x28 backend=(PIE|scalar) .+')
MEASURE = re.compile(
    rb'GRID_APP MEASURE ([3-5]) repeats=(\d+) scalar_us=(\d+) pie_us=(\d+) equal=([01])')
ROUTES = re.compile(rb'GRID_APP ROUTES ([3-5]) gather_us=(\d+) affine_us=(\d+) equal=([01])')
FINAL_MEM = re.compile(rb'MEM free=(\d+) largest=(\d+) js=(\d+) frames=(\d+)')


def payload(line):
    """Keep marker matching exact while accepting ESP-IDF prefixes and colors."""
    return PREFIX.sub(b'', ANSI.sub(b'', line).rstrip(b'\r'), count=1)


def check_faults(raw):
    match = FAULT.search(ANSI.sub(b'', raw))
    if match:
        raise RuntimeError('Device fault marker: ' + match.group().decode('ascii', 'replace'))


def complete_lines(raw):
    check_faults(raw)
    # An unterminated marker is never evidence, even if all digits are present.
    return [payload(line) for line in raw.split(b'\n')[:-1]]


def check_grid_log(raw):
    """Validate one ordinary automatic cycle; report observations, not a win."""
    lines = complete_lines(raw)
    arms, pipes, memories, modes, equality = {}, {}, {}, {}, {}
    events = []
    for position, line in enumerate(lines):
        for name, pattern, destination in (
                ('ARM', ARM, arms), ('PIPE', PIPE, pipes), ('MEM', MEM, memories)):
            if line.startswith(b'GRID_APP ' + name.encode() + b' '):
                match = pattern.fullmatch(line)
                if not match:
                    raise RuntimeError('Malformed or truncated GRID ' + name + ' line')
                arm = int(match[1])
                if arm in destination:
                    raise RuntimeError('Duplicate GRID ' + name + ' arm ' + str(arm))
                destination[arm] = (position, match, line)
                events.append((name, arm))
        if re.match(rb'GRID_APP MODE [6-9](?: |$)', line):
            match = MODE.fullmatch(line)
            if not match or int(match[1]) in modes:
                raise RuntimeError('Malformed or duplicate GRID pipeline MODE')
            modes[int(match[1])] = (position, match[2])
        for name, pattern in (('MEASURE', MEASURE), ('ROUTES', ROUTES)):
            if line.startswith(b'GRID_APP ' + name.encode() + b' '):
                match = pattern.fullmatch(line)
                if not match:
                    raise RuntimeError('Malformed or truncated GRID ' + name + ' equality')
                mode = int(match[1])
                key = (name, mode)
                if key in equality or match[match.lastindex] != b'1':
                    raise RuntimeError('Duplicate or unequal GRID ' + name + ' mode ' + str(mode))
                if name == 'MEASURE' and int(match[2]) != 8:
                    raise RuntimeError('Unexpected GRID equality repeat count')
                equality[key] = position
    if events != [(kind, arm) for arm in range(4) for kind in ('ARM', 'PIPE', 'MEM')]:
        raise RuntimeError('Four complete, ordered GRID ARM/PIPE/MEM sets are required')
    if set(modes) != {6, 7, 8, 9}:
        raise RuntimeError('Four GRID pipeline MODE markers are required')
    if set(equality) != {(kind, mode) for mode in (3, 4, 5) for kind in ('MEASURE', 'ROUTES')}:
        raise RuntimeError('GRID MEASURE and ROUTES equality modes 3, 4 and 5 are required')
    if any(position >= arms[0][0] for position in equality.values()):
        raise RuntimeError('GRID equality must precede the pipeline arms')
    observations = []
    pipe_names = ('frames', 'source_us', 'run_us', 'native_copy_us', 'bind_us',
                  'kernel_us', 'native_total_us', 'native_runs')
    mem_names = ('native_max_us', 'heap_free', 'heap_largest', 'heap_min_free',
                 'max_gap_us', 'gaps_over_50ms', 'gaps_over_75ms')
    for arm, labels in enumerate(EXPECTED_ARMS):
        arm_position, arm_match, arm_line = arms[arm]
        pipe_position, pipe_match, pipe_line = pipes[arm]
        _, memory_match, memory_line = memories[arm]
        mode_position, backend = modes[arm + 6]
        expected_backend = b'PIE' if labels[1] == 'AUTO' else b'scalar'
        if (tuple(value.decode() for value in arm_match.group(2, 3)) != labels or
                tuple(value.decode() for value in pipe_match.group(2, 3)) != labels or
                backend != expected_backend or not arm_position < mode_position < pipe_position):
            raise RuntimeError('GRID arm ' + str(arm) + ' route, layout, backend or ordering mismatch')
        values = dict(zip(pipe_names, map(int, pipe_match.group(*range(4, 12)))))
        values.update(zip(mem_names, map(int, memory_match.group(*range(2, 9)))))
        if values['frames'] != 180 or values['native_runs'] != 180:
            raise RuntimeError('GRID arm ' + str(arm) + ' did not complete 180 normal frames')
        if not 0 < values['heap_largest'] <= values['heap_free'] or not values['heap_min_free']:
            raise RuntimeError('GRID arm ' + str(arm) + ' has invalid memory observations')
        if (values['native_copy_us'] + values['bind_us'] + values['kernel_us'] >
                values['native_total_us'] or values['native_total_us'] > values['run_us']):
            raise RuntimeError('GRID arm ' + str(arm) + ' has inconsistent timing observations')
        observations.append(dict(arm=arm, layout=labels[0], route=labels[1],
                                 backend=backend.decode(), **values,
                                 arm_line=arm_line.decode(), pipe_line=pipe_line.decode(),
                                 memory_line=memory_line.decode()))
    return {'status': 'passed', 'grid_measure_and_route_equality': 'passed',
            'equality_modes': [3, 4, 5], 'arms': observations,
            'physical_checks': 'not assessed', 'performance_claim': 'none'}


class Session:
    """Line framing keeps coalesced read suffixes for the next wait."""
    def __init__(self, port, logfile, clock=time.monotonic):
        self.port, self.logfile, self.clock = port, logfile, clock
        self.deadline = clock() + 540
        self.raw = bytearray()
        self.pending = b''
        self.pending_start = 0
        self.lines = []
        self.cursor = 0

    def read(self):
        incoming = self.port.read(32768)
        if not incoming:
            return
        # Preserve the fault and partial bytes before trying to interpret them.
        self.logfile.write(incoming)
        self.logfile.flush()
        self.raw.extend(incoming)
        self.pending += incoming
        check_faults(self.pending)
        if len(self.raw) > 16 * 1024 * 1024:
            raise RuntimeError('Serial log exceeded the bounded ordinary-run size')
        while b'\n' in self.pending:
            line, self.pending = self.pending.split(b'\n', 1)
            self.lines.append((self.pending_start, payload(line)))
            self.pending_start += len(line) + 1
        if len(self.pending) > 65536:
            raise RuntimeError('Serial line exceeded the bounded line size')

    def collect(self, marker, timeout=12, after=None):
        pattern = re.compile(marker)
        deadline = min(self.clock() + timeout, self.deadline)
        received = []
        while self.clock() < deadline:
            while self.cursor < len(self.lines):
                offset, line = self.lines[self.cursor]
                self.cursor += 1
                if after is not None and offset < after:
                    continue
                received.append(line + b'\n')
                if pattern.fullmatch(line):
                    return b''.join(received)
            self.read()
        raise RuntimeError('Timeout waiting for ' + repr(marker) + '; inspect serial.log')

    def command(self, key, marker, timeout=12):
        if key not in SAFE_KEYS:
            raise ValueError('Only ordinary HELLO/GRID menu and input keys are allowed')
        if self.clock() >= self.deadline:
            raise RuntimeError('Overall device check deadline exceeded')
        # Old coalesced lines stay logged, but cannot acknowledge a new key.
        after = len(self.raw)
        if self.port.write(key) != len(key):
            raise RuntimeError('Incomplete serial key write')
        return self.collect(marker, timeout, after=after)

    def settle(self):
        # Observe trailing errors after HOME without sending a recovery command.
        deadline = min(self.clock() + 0.3, self.deadline)
        while self.clock() < deadline:
            self.read()
        if ANSI.sub(b'', self.pending).strip():
            raise RuntimeError('Incomplete final serial line; inspect serial.log')


def require_identity(raw, expected):
    identities = [line for line in complete_lines(raw) if line.startswith(b'APP_ID')]
    if identities != [b'APP_ID ' + expected]:
        raise RuntimeError('App identity missing, ambiguous or wrong; expected ' + expected.decode())


def final_memory(raw):
    lines = complete_lines(raw)
    candidates = [(index, line) for index, line in enumerate(lines) if line.startswith(b'MEM ')]
    stops = [index for index, line in enumerate(lines) if line == b'APP_STOPPED']
    homes = [index for index, line in enumerate(lines) if line == b'HOME_READY']
    if len(candidates) != 1 or len(stops) != 1 or len(homes) != 1:
        raise RuntimeError('Complete final MEM, APP_STOPPED and HOME_READY sequence required')
    index, line = candidates[0]
    match = FINAL_MEM.fullmatch(line)
    if not match or not index < stops[0] < homes[0]:
        raise RuntimeError('Final MEM line is incomplete or teardown markers are out of order')
    values = dict(zip(('free', 'largest', 'js', 'frames'), map(int, match.groups())))
    if not 0 < values['largest'] <= values['free'] or values['js'] != 0 or values['frames'] < 1:
        raise RuntimeError('Final MEM does not describe a completed app teardown')
    return dict(values, line=line.decode())


def select_first_app(session):
    session.command(b'q', rb'HOME_READY')
    category = session.command(b'a', rb'CATEGORY \d+')
    if complete_lines(category)[-1] != b'CATEGORY 0':
        raise RuntimeError('Apps category 0 was not selected')
    previous = None
    for _ in range(APP_ROWS):
        row = session.command(b'u', rb'APP \d+')
        current = int(complete_lines(row)[-1].split()[1])
        if current >= APP_ROWS or (previous is not None and current != max(0, previous - 1)):
            raise RuntimeError('Unexpected app-row navigation acknowledgement')
        previous = current
    if previous != 0:
        raise RuntimeError('Could not select HELLO row 0')


def run_smoke(port, logfile, cycles, grid=False, clock=time.monotonic, result=None):
    if isinstance(cycles, bool) or not isinstance(cycles, int) or not 1 <= cycles <= 20:
        raise ValueError('cycles must be between 1 and 20')
    result = {} if result is None else result
    result.update(schema='pocketjs-integrated-smoke-v1', status='failed', cycles=cycles,
                  hello_completed=0, grid_requested=bool(grid), post_stop_memory=[],
                  memory_note='Observations only; Wi-Fi/autosync and device state affect these values',
                  physical_checks='not assessed', performance_claim='none')
    session = Session(port, logfile, clock)
    result['phase'] = 'hello-navigation'
    select_first_app(session)
    identity_start = len(session.raw)
    for cycle in range(cycles):
        result['phase'] = 'hello-cycle-' + str(cycle + 1)
        launch_start = len(session.raw)
        started = session.command(b'e', rb'(?:HELLO|KASANE)_FRAME_PRESENTED', timeout=20)
        require_identity(bytes(session.raw[launch_start:]), b'local.hello')
        if b'HELLO_READY' not in complete_lines(started):
            raise RuntimeError('HELLO_READY startup marker missing')
        session.command(b'e', rb'HELLO_COUNT 1')
        stopped = session.command(b'q', rb'HOME_READY')
        result['post_stop_memory'].append(final_memory(stopped))
        result['hello_completed'] += 1
    if grid:
        result['phase'] = 'grid-navigation'
        for index in range(1, GRID_ROW + 1):
            row = session.command(b'd', rb'APP \d+')
            if complete_lines(row)[-1] != b'APP ' + str(index).encode():
                raise RuntimeError('Could not select embedded GRID LAB row 9')
        result['phase'] = 'grid-pipeline'
        launch_start = len(session.raw)
        session.command(b'e', rb'GRID_APP MODE 0 .+', timeout=20)
        require_identity(bytes(session.raw[launch_start:]), b'local.gridlab')
        session.collect(MEM.pattern.replace(b'([0-3])', b'(3)', 1), timeout=120)
        # Verify the run before allowing any further command, including normal exit.
        result['grid'] = check_grid_log(bytes(session.raw[launch_start:]))
        result['phase'] = 'grid-stop'
        stopped = session.command(b'q', rb'HOME_READY')
        result['grid']['post_stop_memory'] = final_memory(stopped)
    result['phase'] = 'final-log-check'
    session.settle()
    lines = complete_lines(bytes(session.raw[identity_start:]))
    identities = [line for line in lines if line.startswith(b'APP_ID')]
    expected = [b'APP_ID local.hello'] * cycles + ([b'APP_ID local.gridlab'] if grid else [])
    if identities != expected:
        raise RuntimeError('Unexpected app identity or extra launch in the serial log')
    result['phase'] = 'complete'
    result['status'] = 'passed'
    return result


def main(argv=None, serial_factory=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--allow-device', action='store_true')
    parser.add_argument('--port')
    parser.add_argument('--out', type=Path)
    parser.add_argument('--cycles', type=int, default=3)
    parser.add_argument('--grid', action='store_true')
    args = parser.parse_args(argv)
    if not args.allow_device or not args.port or not args.port.strip() or not args.out:
        parser.error('--allow-device, --port and --out are all required; no default port')
    if not 1 <= args.cycles <= 20:
        parser.error('--cycles must be between 1 and 20')
    args.out.mkdir(parents=True, exist_ok=False)
    result = {'status': 'failed', 'error': 'interrupted before completion',
              'physical_checks': 'not assessed', 'performance_claim': 'none'}
    code = 1
    try:
        with (args.out / 'serial.log').open('wb') as logfile:
            if serial_factory is None:
                # Offline import/parser tests require neither pyserial nor a port.
                import serial
                serial_factory = serial.Serial
            port = serial_factory(port=None, baudrate=115200, timeout=0.1, write_timeout=2)
            port.dtr = False
            port.rts = False
            port.port = args.port
            with port:
                run_smoke(port, logfile, args.cycles, grid=args.grid, result=result)
        result.pop('error', None)
        code = 0
    except (Exception, KeyboardInterrupt) as error:
        result.update(status='failed', error=str(error) or type(error).__name__)
        code = 130 if isinstance(error, KeyboardInterrupt) else 1
    finally:
        (args.out / 'result.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result, indent=2))
    return code


if __name__ == '__main__':
    raise SystemExit(main())
