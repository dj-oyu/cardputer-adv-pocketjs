"""Kasane-only system acceptance suite; never builds the legacy UI/Taffy graph."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', type=Path, default=Path('build_system_full'))
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--nm', help='ESP32-S3 nm executable; required for full run')
    p.add_argument('--port', help='serial port; full run flashes this device')
    p.add_argument('--flash', action='store_true', help='authorize flashing the just-audited firmware')
    p.add_argument('--host-only', action='store_true', help='partial run; never reports FULL PASS')
    p.add_argument('--cycles', type=int, default=100)
    args = p.parse_args()
    if sys.flags.optimize:
        p.error('Python assertions must be enabled; do not use -O or PYTHONOPTIMIZE')
    if args.cycles < 2:
        p.error('--cycles must be at least 2')
    if not args.host_only and (not args.nm or not args.port or not args.flash):
        p.error('full run requires --nm, --port and --flash')
    out = args.out.resolve()
    if out.exists() and any(out.iterdir()):
        p.error('--out must be new or empty; preserve previous evidence')
    out.mkdir(parents=True, exist_ok=True)
    build = args.build.resolve()
    report = {'schema': 1, 'status': 'RUNNING', 'mode': 'host' if args.host_only else 'full',
              'cycles': args.cycles, 'stages': [],
              'scope': 'System services, Kasane native/JS, legacy exclusion, device composition and teardown',
              'not_measured': ['physical LCD/audio output', 'live SNTP clock jumps',
                               'sleep current', 'all peripherals and filesystem failure modes']}
    flashed = False

    def save():
        (out / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')

    def stage(name, command, cwd=ROOT):
        entry = {'name': name, 'command': list(map(str, command)), 'status': 'RUNNING'}
        report['stages'].append(entry)
        save()
        began = time.monotonic()
        print('SYSTEM_FULL START', name, flush=True)
        with (out / (name + '.log')).open('w', encoding='utf-8') as log:
            result = subprocess.run(command, cwd=cwd, stdout=log, stderr=subprocess.STDOUT)
        entry.update(seconds=round(time.monotonic()-began, 3), exit_code=result.returncode,
                     status='PASS' if result.returncode == 0 else 'FAIL')
        save()
        if result.returncode:
            raise RuntimeError(f'{name} failed; see {out / (name + ".log")}')
        print('SYSTEM_FULL PASS', name, flush=True)

    def host(name, script):
        if os.name == 'nt':
            path = ROOT.as_posix()
            path = '/mnt/' + path[0].lower() + path[2:]
            stage(name, ['wsl', 'bash', '-lc', 'cd ' + shlex.quote(path) + ' && ' + script])
        else:
            stage(name, ['bash', '-lc', script])

    try:
        report['commit'] = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
        report['working_tree'] = subprocess.check_output(['git', 'status', '--short'], cwd=ROOT, text=True)
        host('system-host', 'bash tools/build_power_test.sh')
        host('kasane-native', 'bash tools/kasane_contract/run.sh')
        host('kasane-js-sanitized', 'bash tools/build_kasane_test.sh && /tmp/test-pocket-kasane')
        host('kasane-js-optimized', 'CFLAGS="-O2 -fstrict-aliasing" OUT=/tmp/test-pocket-kasane-o2 '
             'bash tools/build_kasane_test.sh && /tmp/test-pocket-kasane-o2')
        if args.host_only:
            report['status'] = 'HOST_PASS_DEVICE_NOT_RUN'
            return 0
        idf = Path(os.environ['IDF_PATH']) / 'tools' / 'idf.py'
        absent = build / 'absent-legacy-ui'
        if absent.exists():
            raise RuntimeError('legacy exclusion sentinel must not exist: ' + str(absent))
        defaults = ROOT / 'tools' / 'system_full_test.defaults'
        # Direct IDF subprocess mode avoids nested asyncio pipe collection
        # hanging after CMake exits on Windows with redirected output.
        stage('firmware-build', [sys.executable, str(idf), '--no-hints', '-B', str(build),
              '-D', 'KSN_ONLY=ON', '-D', 'POCKETJS_SOURCE_DIR=' + absent.as_posix(),
              '-D', 'SDKCONFIG=' + (build / 'sdkconfig').as_posix(),
              '-D', 'SDKCONFIG_DEFAULTS=' + (ROOT / 'sdkconfig.defaults').as_posix() + ';' + defaults.as_posix(),
              'build'])
        config = (build / 'sdkconfig').read_text()
        if 'CONFIG_KSN_DEVICE_PROBE=y' not in config or 'CONFIG_SPIRAM=y' in config:
            raise RuntimeError('requires native probe enabled and PSRAM disabled')
        stage('taffy-exclusion', [sys.executable, 'tools/check_kasane_link.py', '--build', str(build), '--nm', args.nm])
        report['firmware_sha256'] = hashlib.sha256((build / 'cardputer_pocketjs.bin').read_bytes()).hexdigest()
        flashed = True  # Attempt cleanup even if flashing fails part way through.
        stage('flash', [sys.executable, '-m', 'esptool', '--chip', 'esp32s3', '--port', args.port,
              '--baud', '460800', '--before', 'default-reset', '--after', 'hard-reset',
              'write-flash', '@flash_args'], cwd=build)
        for name, script, extra in [
            ('device-animation', 'kasane_device_test.py', ['--ticks', '300']),
            ('device-system', 'system_device_test.py', ['--kasane']),
            ('device-lifetime', 'kasane_only_device_test.py', ['--cycles', str(args.cycles)])]:
            stage(name, [sys.executable, 'tools/' + script, '--port', args.port,
                         '--out', str(out / name), *extra])
        report['memory'] = json.loads((out / 'device-lifetime' / 'memory.json').read_text())
        report['status'] = 'FULL_PASS'
        return 0
    except (Exception, KeyboardInterrupt) as error:
        report['status'] = 'FAIL'
        report['error'] = str(error) or type(error).__name__
        print('SYSTEM_FULL FAIL', report['error'], file=sys.stderr, flush=True)
        return 1
    finally:
        if flashed:
            try:
                import serial
                with serial.Serial(args.port, 115200, timeout=0.1) as port:
                    port.write(b'Zq')
                    time.sleep(0.5)
                report['cleanup'] = 'probe owner released; HOME requested'
            except Exception as error:
                report['cleanup_error'] = str(error)
                report['status'] = 'FAIL'
        save()
        print('SYSTEM_FULL RESULT', report['status'], str(out / 'report.json'), flush=True)
        if report['status'] == 'FAIL':
            return 1


if __name__ == '__main__':
    code = main()
    sys.exit(code)
