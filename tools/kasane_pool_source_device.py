"""Exercise diagnostic 0: a real FreeRTOS producer updates a Kasane source."""
import argparse
import json
from pathlib import Path
import re
import time

import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--no-capture', action='store_true',
                        help='exercise source lifetime without LCD capture allocations')
    parser.add_argument('--hold-seconds', type=float, default=2,
                        help='no-capture observation duration (default: 2)')
    args = parser.parse_args()
    if args.hold_seconds < 1 or args.hold_seconds > 120:
        parser.error('--hold-seconds must be 1..120')
    if args.out.exists() and any(args.out.iterdir()):
        parser.error('--out must be new or empty')
    args.out.mkdir(parents=True, exist_ok=True)
    lines = []
    with (args.out / 'serial.log').open('w', encoding='utf-8') as log:
        port = serial.Serial(port=None, baudrate=115200, timeout=0.15)
        port.dtr = False
        port.rts = False
        port.port = args.port
        port.open()
        try:
            def read_line():
                line = port.readline().decode(errors='replace').strip()
                if line:
                    lines.append(line)
                    log.write(line + '\n')
                return line

            def until(marker, seconds):
                deadline = time.monotonic() + seconds
                while time.monotonic() < deadline:
                    line = read_line()
                    if marker in line:
                        print('SEEN', line, flush=True)
                        return line
                    if ('APP_FAILED' in line or 'KSN_POOL_SOURCE ERROR' in line or
                            'panic' in line.lower()):
                        raise RuntimeError(line)
                raise TimeoutError(f'{marker}: {lines[-8:]}')

            def capture(name):
                port.write(b's')
                until('CAPTURE_BEGIN', 10)
                rows = {}
                deadline = time.monotonic() + 20
                while time.monotonic() < deadline:
                    line = read_line()
                    match = re.search(r'PIX (\d+) ([0-9a-f]{960})', line)
                    if match:
                        rows[int(match.group(1))] = bytes.fromhex(match.group(2))
                    if 'CAPTURE_END' in line:
                        break
                if len(rows) != 135 or any(len(row) != 480 for row in rows.values()):
                    raise RuntimeError(f'{name}: incomplete LCD capture ({len(rows)} rows)')
                pixels = b''.join(rows[y] for y in range(135))
                (args.out / f'{name}.rgb565').write_bytes(pixels)
                print('CAPTURE', name, len(pixels), flush=True)
                return pixels

            port.write(b'q')
            until('HOME_READY', 12)
            port.write(b'0')
            until('KSN_POOL_SOURCE BOUND', 15)
            changed = None
            outside = None
            if args.no_capture:
                deadline = time.monotonic() + args.hold_seconds
                while time.monotonic() < deadline:
                    read_line()
            else:
                before = capture('before')
                after = capture('after')
                changed = 0
                outside = 0
                for y in range(135):
                    for x in range(240):
                        at = 2 * (y * 240 + x)
                        if before[at:at + 2] != after[at:at + 2]:
                            changed += 1
                            if x >= 96 or y >= 24:
                                outside += 1
            port.write(b'q')
            stop = until('KSN_POOL: STOP published=', 12)
            until('APP_STOPPED', 12)
            match = re.search(r'published=(\d+) skipped=(\d+)', stop)
            if not match:
                raise RuntimeError(f'unexpected stop line: {stop}')
            summary = {'binary_probe': '0', 'changed_pixels': changed,
                       'outside_source_pixels': outside,
                       'published': int(match.group(1)), 'skipped': int(match.group(2)),
                       'errors': [line for line in lines if 'APP_FAILED' in line or
                                  'KSN_POOL_SOURCE ERROR' in line or
                                  'panic' in line.lower()]}
            (args.out / 'summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
            if ((changed is not None and (changed == 0 or outside != 0)) or
                    summary['published'] < 2 or summary['errors']):
                raise RuntimeError(f'pool-source pixel gate: {summary}')
            print('POOL_SOURCE_' + ('LIFETIME' if args.no_capture else 'PIXELS') +
                  ' PASS', summary, flush=True)
        finally:
            port.close()


if __name__ == '__main__':
    main()
