"""Capture deterministic hello states for pre/dirty-node LCD pixel comparison.

Flashing and restoring the original app partition are deliberately separate.
The 's' command forces a complete LCD redraw, so every capture has 240x135
RGB565 pixels even when the renderer normally updates only dirty bands.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import time

import serial


TARGET_COUNTS = (0, 1, 2, 9, 10, 99, 100, 180)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    if args.out.exists() and any(args.out.iterdir()):
        parser.error('--out must be new or empty')
    args.out.mkdir(parents=True, exist_ok=True)
    events = []
    captures = {}

    with serial.Serial(args.port, 115200, timeout=0.15) as port:
        def until(pattern, seconds=12):
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                line = port.readline().decode(errors='replace').strip()
                if not line:
                    continue
                events.append(line)
                if any(bad in line for bad in ('Guru Meditation', 'APP_FAILED', 'START_FAILED')):
                    raise RuntimeError(line)
                if re.search(pattern, line):
                    return line
            raise TimeoutError(f'{pattern}: {events[-8:]}')

        def key(value, pattern, seconds=12):
            port.write(value.encode('ascii'))
            result = until(pattern, seconds)
            print('SEEN', result, flush=True)
            return result

        def capture(count):
            port.write(b's')
            raw = bytearray()
            deadline = time.monotonic() + 20
            while b'CAPTURE_END' not in raw and time.monotonic() < deadline:
                raw.extend(port.read(32768))
            if b'CAPTURE_BEGIN 240 135' not in raw or b'CAPTURE_END' not in raw:
                raise RuntimeError(f'count {count}: missing capture delimiters')
            rows = {}
            for y, pixels in re.findall(rb'PIX (\d+) ([0-9a-f]{960})', raw):
                index = int(y)
                if index in rows:
                    raise RuntimeError(f'count {count}: duplicate row {index}')
                rows[index] = bytes.fromhex(pixels.decode('ascii'))
            if set(rows) != set(range(135)):
                raise RuntimeError(f'count {count}: incomplete rows ({len(rows)})')
            image = b''.join(rows[y] for y in range(135))
            if len(image) != 240 * 135 * 2:
                raise RuntimeError(f'count {count}: wrong image size')
            (args.out / f'hello-{count:03}.rgb565').write_bytes(image)
            captures[str(count)] = hashlib.sha256(image).hexdigest()
            print('CAPTURE', count, captures[str(count)], flush=True)

        try:
            key('q', r'HOME_READY')
            key('a', r'CATEGORY 0')
            key('e', r'KASANE_FRAME_PRESENTED')
            capture(0)
            for count in range(1, TARGET_COUNTS[-1] + 1):
                port.write(b'e')
                until(rf'HELLO_COUNT {count}\b', 6)
                if count in TARGET_COUNTS:
                    capture(count)
            key('q', r'APP_STOPPED')
        finally:
            (args.out / 'events.log').write_text('\n'.join(events) + '\n', encoding='utf-8')

    (args.out / 'summary.json').write_text(
        json.dumps({'counts': captures, 'events': len(events)}, indent=2), encoding='utf-8')
    print('HELLO_PIXELS PASS', len(captures), flush=True)


if __name__ == '__main__':
    main()
