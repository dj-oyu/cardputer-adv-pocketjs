"""Host-side A/B of the Kasane render switches, on the real device.

Run this on the machine that can see the board (the ESP-IDF env loaded, pyserial
available), with the device free:

    python tools/host_kasane_opt_ab.py --out .cache/opt-ab --port COM3 --flash

It builds one binary with -DKASANE_AB=1 (main/app_session.c rotates the render
switches one window at a time: arm 0 is all switches on, arm r+1 turns switch r
off and the rest on), flashes it, drives the same key sequence
tools/benchmark_app.py uses, and pairs the windows so every switch gets an
off/on comparison inside ONE binary -- the only way the difference is the
switch and not build-to-build instruction-cache placement
(docs/perf/pie-simd.md sections 6.2/6.3).

What it writes to --out: report.json (commit, build, per-switch medians and the
paired deltas, arm sample counts) plus raw.log (every serial line). It exits
non-zero when an arm never appeared or a switch has too few samples, so a red
result is a measurement failure, not a silent zero.

The device-side numbers it prints are the evidence the optimisation decision
needs; the host-side pixel proofs are in tools/kasane_contract/run.sh and the
per-kernel harnesses (tools/pie/).
"""
import argparse
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
AB_RE = re.compile(r'AB arm=(?P<arm>\d+)\s+(?P<states>[^ ]+(?: [^ ]+)*?)\s+'
                   r'turn_ms=(?P<turn>[\d.]+) render_ms=(?P<render>[\d.]+) '
                   r'send_ms=(?P<send>[\d.]+)')
STATE_RE = re.compile(r'([A-Za-z_][A-Za-z0-9_]*)=(\d+)')


def sh(cmd, cwd=ROOT, log=None):
    if log:
        with log.open('a', encoding='utf-8') as fh:
            fh.write('$ ' + ' '.join(map(str, cmd)) + '\n')
            fh.flush()
            return subprocess.run([str(c) for c in cmd], cwd=str(cwd), stdout=fh,
                                  stderr=subprocess.STDOUT).returncode
    return subprocess.run([str(c) for c in cmd], cwd=str(cwd)).returncode


def idf_py():
    idf = os.environ.get('IDF_PATH')
    if not idf:
        sys.exit('IDF_PATH is not set: load the ESP-IDF environment first')
    return Path(idf) / 'tools' / 'idf.py'


def drive(port, out, minutes):
    """Home, open the sample app, then let the app run while the windows rotate."""
    import serial  # imported here so --help works without pyserial

    lines = []
    with serial.Serial(port, 115200, timeout=0.05) as s, \
            (out / 'raw.log').open('w', encoding='utf-8') as raw:
        s.reset_input_buffer()

        def wait(marker, limit=8.0, press=None):
            end = time.monotonic() + limit
            while time.monotonic() < end:
                if press and time.monotonic() > end - limit / 2:
                    s.write(press)
                    press = None
                text = s.readline().decode('utf-8', 'replace').rstrip('\r\n')
                if text:
                    raw.write(text + '\n')
                    raw.flush()
                    lines.append(text)
                    if marker in text:
                        return True
            return False

        s.write(b'q')
        wait('HOME_READY')
        s.write(b'a')
        wait('CATEGORY 0')
        s.write(b'e')
        wait('HELLO_FRAME_PRESENTED')
        deadline = time.monotonic() + minutes * 60.0
        while time.monotonic() < deadline:
            s.write(b'e')          # keep the app's frame pump busy
            wait('PAINT', limit=0.6)
        s.write(b'q')
        wait('HOME_READY')
    return lines


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', type=Path, required=True, help='new or empty directory for the evidence')
    p.add_argument('--port', required=True, help='serial port, e.g. COM3 or /dev/ttyACM0')
    p.add_argument('--build', type=Path, default=Path('build_ab'))
    p.add_argument('--flash', action='store_true', help='build and flash before measuring')
    p.add_argument('--minutes', type=float, default=4.0, help='how long to let the windows rotate')
    p.add_argument('--min-samples', type=int, default=3, help='windows per arm required')
    args = p.parse_args()

    out = args.out.resolve()
    if out.exists() and any(out.iterdir()):
        p.error('--out must be new or empty; preserve previous evidence')
    out.mkdir(parents=True, exist_ok=True)
    log = out / 'commands.log'
    report = {'schema': 1, 'status': 'RUNNING', 'port': args.port, 'build': str(args.build),
              'not_measured': ['physical LCD/audio output', 'host-side pixel proofs (see run.sh)']}
    (out / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    try:
        report['commit'] = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT,
                                                   text=True, shell=False).strip()
        report['branch'] = subprocess.check_output(['git', 'rev-parse', '--abbrev-ref', 'HEAD'],
                                                   cwd=ROOT, text=True).strip()
        report['working_tree'] = subprocess.check_output(['git', 'status', '--short'], cwd=ROOT,
                                                         text=True).strip()
        if args.flash:
            rc = sh([idf_py(), '-B', args.build, '-D', 'CMAKE_C_FLAGS=-DKASANE_AB=1', 'build'], log=log)
            if rc:
                raise RuntimeError('build failed; see commands.log')
            rc = sh([idf_py(), '-B', args.build, '-p', args.port, 'flash'], log=log)
            if rc:
                raise RuntimeError('flash failed; see commands.log')
        lines = drive(args.port, out, args.minutes)
        report['serial_lines'] = len(lines)

        arms = {}
        states_by_arm = {}
        for line in lines:
            m = AB_RE.search(line)
            if not m:
                continue
            arm = int(m.group('arm'))
            states = dict((k, int(v)) for k, v in STATE_RE.findall(m.group('states')))
            states_by_arm[arm] = states
            arms.setdefault(arm, []).append({'turn_ms': float(m.group('turn')),
                                             'render_ms': float(m.group('render')),
                                             'send_ms': float(m.group('send'))})
        if not arms:
            raise RuntimeError('no AB arm lines on the wire: was the binary built with '
                               '-DKASANE_AB=1 and did the app run?')
        switches = sorted(states_by_arm[max(states_by_arm)])            # arm with most states
        report['switches'] = switches
        report['arms'] = {str(a): {'windows': len(v),
                                   'render_ms_median': statistics.median(x['render_ms'] for x in v),
                                   'turn_ms_median': statistics.median(x['turn_ms'] for x in v)}
                          for a, v in sorted(arms.items())}

        # Arm 0 is all-on; arm r+1 turns switch r off. The paired difference is the
        # switch's own cost, window by window, so a drifting clock cancels.
        def med_by_arm(arm, key):
            return [x[key] for x in arms.get(arm, [])]

        table = []
        problems = []
        for r, name in enumerate(switches):
            off, on = r + 1, 0
            if len(arms.get(off, [])) < args.min_samples or len(arms.get(on, [])) < args.min_samples:
                problems.append(f'{name}: too few windows (on={len(arms.get(on, []))}, '
                                f'off={len(arms.get(off, []))}, need {args.min_samples})')
                table.append({'switch': name, 'status': 'INSUFFICIENT'})
                continue
            on_med = statistics.median(med_by_arm(on, 'render_ms'))
            off_med = statistics.median(med_by_arm(off, 'render_ms'))
            table.append({'switch': name, 'status': 'OK',
                          'render_ms_on_median': round(on_med, 3),
                          'render_ms_off_median': round(off_med, 3),
                          'render_ms_delta_off_minus_on': round(off_med - on_med, 3),
                          'turn_ms_on_median': round(statistics.median(med_by_arm(on, 'turn_ms')), 3),
                          'turn_ms_off_median': round(statistics.median(med_by_arm(off, 'turn_ms')), 3),
                          'windows_on': len(arms[on]), 'windows_off': len(arms[off])})
        report['table'] = table
        report['suggested_range_of_used_window'] = 'see turn_ms: it is the frame budget the app had'
        if problems:
            report['problems'] = problems
            report['status'] = 'INSUFFICIENT'
        else:
            report['status'] = 'PASS'
        for row in table:
            if row['status'] != 'OK':
                print('AB', row['switch'], row['status'], flush=True)
            else:
                print(f"AB {row['switch']:<18} render_ms on={row['render_ms_on_median']:.3f} "
                      f"off={row['render_ms_off_median']:.3f} "
                      f"delta={row['render_ms_delta_off_minus_on']:+.3f} "
                      f"(windows {row['windows_on']}/{row['windows_off']})", flush=True)
        return 0
    except (Exception, KeyboardInterrupt) as error:
        report['status'] = 'FAIL'
        report['error'] = str(error) or type(error).__name__
        print('OPT_AB FAIL', report['error'], file=sys.stderr, flush=True)
        return 1
    finally:
        (out / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
        print('OPT_AB RESULT', report['status'], str(out / 'report.json'), flush=True)


if __name__ == '__main__':
    sys.exit(main())
