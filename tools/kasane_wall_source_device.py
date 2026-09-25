"""Exercise diagnostic 7: capture a real wall-source-driven minute change."""
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
    args = parser.parse_args()
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
                    if ('APP_FAILED' in line or 'KSN_WALL_SOURCE ERROR' in line or
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
            port.write(b'7')
            bound = until('KSN_WALL_SOURCE BOUND unixMs=', 15)
            unix_ms = bound.split('unixMs=', 1)[1].strip()
            time.sleep(0.5)
            before = capture('before')
            # The C producer must advance at least once without a JS set().
            until_time = time.monotonic() + 65
            while time.monotonic() < until_time:
                read_line()
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
            until('APP_STOPPED', 12)
            summary = {'binary_probe': '7', 'bound_unix_ms': unix_ms,
                       'changed_pixels': changed, 'outside_clock_pixels': outside,
                       'errors': [line for line in lines if 'APP_FAILED' in line or
                                  'KSN_WALL_SOURCE ERROR' in line or 'panic' in line.lower()]}
            (args.out / 'summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
            if changed == 0 or outside != 0 or summary['errors']:
                raise RuntimeError(f'wall-source pixel gate: {summary}')
            print('WALL_SOURCE_PIXELS PASS', summary, flush=True)
        finally:
            port.close()


if __name__ == '__main__':
    main()
