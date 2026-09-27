"""Run P0-only mutex-arena race or allocation-failure diagnostics after boot."""

import argparse
from pathlib import Path
import time

import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', default='COM3')
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--mode', choices=('race', 'oom'), default='race')
    args = parser.parse_args()
    if args.out.exists():
        parser.error('--out must be a new file')
    args.out.parent.mkdir(parents=True, exist_ok=True)

    port = serial.Serial(port=None, baudrate=115200, timeout=0.1)
    port.dtr = False
    port.rts = False
    port.port = args.port
    lines = []

    def until(markers, seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            line = port.readline().decode(errors='replace').rstrip('\r\n')
            if line:
                lines.append(line)
                if any(marker in line for marker in markers):
                    print(line, flush=True)
                    return line
        raise TimeoutError(f'{markers}: {lines[-8:]}')

    try:
        with port:
            port.write(b'q')
            until(('HOME_READY',), 12)
            port.write(b';' if args.mode == 'race' else b':')
            marker = 'RACE' if args.mode == 'race' else 'OOM_INJECT'
            result = until((marker + '_PASS', marker + '_FAIL',
                            'RACE_SKIP' if args.mode == 'race' else 'OOM_SKIP'), 6)
            if marker + '_PASS' not in result:
                raise RuntimeError(result)
    finally:
        args.out.write_text('\n'.join(lines) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
