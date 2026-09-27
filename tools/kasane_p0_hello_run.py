"""Reproducible Cardputer hello timing run; never writes flash or SD."""

import argparse
import json
from pathlib import Path
import time

import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--hold-seconds', type=float, default=2)
    parser.add_argument('--updates', type=int, default=180)
    args = parser.parse_args()
    if args.hold_seconds < 1 or not 1 <= args.updates <= 300:
        parser.error('hold-seconds must be at least 1 and updates must be 1..300')
    if args.out.exists() and any(args.out.iterdir()):
        parser.error('--out must be a new or empty directory')
    args.out.mkdir(parents=True, exist_ok=True)

    lines = []
    marks = []
    with (args.out / 'serial.log').open('w', encoding='utf-8') as log:
        port = serial.Serial(port=None, baudrate=115200, timeout=0.1)
        port.dtr = False
        port.rts = False
        port.port = args.port
        port.open()
        try:
            def collect(seconds, marker=None, announce=True):
                deadline = time.monotonic() + seconds
                while time.monotonic() < deadline:
                    raw = port.readline()
                    if not raw:
                        continue
                    line = raw.decode(errors='replace').rstrip('\r\n')
                    lines.append(line)
                    log.write(line + '\n')
                    if marker and marker in line:
                        log.flush()
                        if announce:
                            print('SEEN', line, flush=True)
                        return line
                log.flush()
                if marker:
                    raise TimeoutError(f'no {marker!r} within {seconds}s')
                return None

            def key(value, announce=True):
                port.write(value.encode('ascii'))
                marks.append({'t': time.monotonic(), 'key': value})
                if announce:
                    print('KEY', value, flush=True)

            key('q')
            collect(12, 'HOME_READY')
            key('a')
            collect(12, 'CATEGORY 0')
            key('e')
            collect(12, 'KASANE_FRAME_PRESENTED')
            collect(args.hold_seconds / 2)
            for count in range(1, args.updates + 1):
                key('e', announce=False)
                collect(5, f'HELLO_COUNT {count}', announce=False)
                if count % 30 == 0:
                    print('UPDATES', count, flush=True)
            collect(args.hold_seconds / 2)
            key('q')
            collect(12, 'APP_STOPPED')
        finally:
            port.close()

    summary = {
        'port': args.port,
        'hold_seconds': args.hold_seconds,
        'updates': args.updates,
        'marks': marks,
        'events': [line for line in lines if 'KSN_P0:' in line or 'HELLO_COUNT' in line],
        'error_count': sum('panic' in line.lower() or 'APP_FAILED' in line for line in lines),
    }
    (args.out / 'summary.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2),
                                           encoding='utf-8')
    print('TRIAL_DONE', args.out, 'errors', summary['error_count'], flush=True)


if __name__ == '__main__':
    main()
