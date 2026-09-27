"""Exercise a one-shot partial LCD send failure in a P2 diagnostic build.

Requires KASANE_P2_REPAIR_PROBE=ON. Does not flash or modify the SD card.
The capture is the RGB565 buffer immediately before SPI, not LCD GRAM readback.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import time

import serial


def pixels(raw):
    rows = {}
    for y, hex_data in re.findall(rb'PIX (\d+) ([0-9a-f]{960})', raw):
        y = int(y)
        if y in rows:
            raise RuntimeError(f'duplicate captured row {y}')
        rows[y] = bytes.fromhex(hex_data.decode('ascii'))
    return rows


def image(rows):
    if set(rows) != set(range(135)):
        raise RuntimeError(f'incomplete captured frame: {len(rows)} rows')
    result = b''.join(rows[y] for y in range(135))
    if len(result) != 64800:
        raise RuntimeError(f'wrong RGB565 bytes: {len(result)}')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    if args.out.exists() and any(args.out.iterdir()):
        parser.error('--out must be new or empty')
    args.out.mkdir(parents=True, exist_ok=True)
    events = []

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
                    print('SEEN', line, flush=True)
                    return line
            raise TimeoutError(f'{pattern}: {events[-8:]}')

        def key(value, pattern):
            port.write(value.encode('ascii'))
            return until(pattern)

        def read_capture(command, seconds=25):
            port.write(command)
            raw = bytearray()
            deadline = time.monotonic() + seconds
            while b'CAPTURE_END' not in raw and time.monotonic() < deadline:
                raw.extend(port.read(32768))
            if b'CAPTURE_BEGIN 240 135' not in raw or b'CAPTURE_END' not in raw:
                raise RuntimeError(f'capture markers missing; tail={bytes(raw[-500:])!r}')
            return bytes(raw)

        try:
            key('q', r'HOME_READY')
            key('a', r'CATEGORY 0')
            key('e', r'KASANE_FRAME_PRESENTED')
            key('e', r'HELLO_COUNT 1\b')
            baseline = image(pixels(read_capture(b's')))
            (args.out / 'baseline.rgb565').write_bytes(baseline)
            print('BASELINE', hashlib.sha256(baseline).hexdigest(), flush=True)

            # The UI task arms the one-shot failure and forces a full redraw.
            # Three 8-row bands reach SPI, the fourth returns KSN_IO, then the
            # next owner turn repairs without an intervening JS key/update.
            raw = read_capture(b'%', 30)
            (args.out / 'fault-serial.log').write_bytes(raw)
            failed = raw.find(b'KSN_P2: INJECT_FAIL')
            repaired = raw.find(b'KSN_P2: REPAIR_OK')
            if failed < 0 or repaired < failed or b'KSN_P2: PARTIAL_FAILED sent=3' not in raw:
                raise RuntimeError('missing injected failure or repair acknowledgement')
            if b'KSN_P2: REPAIR_OK bands=17 bytes=64800' not in raw:
                raise RuntimeError('repair was not a complete 17-band transfer')
            if b'LCD transfer failed; retaining display work for retry' not in raw:
                raise RuntimeError('Kasane did not report LCD retry path')
            if b'HELLO_COUNT' in raw:
                raise RuntimeError('guest updated the hello slot during repair')
            before_rows = pixels(raw[:failed])
            if set(before_rows) != set(range(24)):
                raise RuntimeError(f'expected first 24 rows before failure, got {sorted(before_rows)}')
            repair = image(pixels(raw[failed:repaired]))
            (args.out / 'repair.rgb565').write_bytes(repair)
            if repair != baseline:
                differences = sum(repair[i:i + 2] != baseline[i:i + 2]
                                  for i in range(0, 64800, 2))
                raise RuntimeError(f'repair differs from baseline at {differences} pixels')
            print('REPAIR', hashlib.sha256(repair).hexdigest(), 'pixels=32400', flush=True)

            after = image(pixels(read_capture(b's')))
            (args.out / 'after.rgb565').write_bytes(after)
            if after != baseline:
                raise RuntimeError('post-repair redraw differs from baseline')
            key('q', r'APP_STOPPED')
        finally:
            (args.out / 'events.log').write_text('\n'.join(events) + '\n', encoding='utf-8')

    (args.out / 'summary.json').write_text(json.dumps({
        'baseline_sha256': hashlib.sha256(baseline).hexdigest(),
        'repair_sha256': hashlib.sha256(repair).hexdigest(),
        'after_sha256': hashlib.sha256(after).hexdigest(),
        'successful_bands_before_failure': 3,
        'repair_rows': 135,
        'mismatched_pixels': 0,
    }, indent=2), encoding='utf-8')
    print('P2_REPAIR PASS', flush=True)


if __name__ == '__main__':
    main()
