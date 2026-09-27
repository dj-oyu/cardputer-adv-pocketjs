"""Verify a music overlay LCD repair against a static help-screen capture.

Requires HOME OVERLAY=music and KASANE_P5_OVERLAY_REPAIR_PROBE=ON. The
RGB565 captures are the bytes immediately before SPI, not LCD GRAM readback.
Never flashes the device or writes to the SD card.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import time

import serial


def rows(raw):
    result = {}
    for y, hex_data in re.findall(rb'PIX (\d+) ([0-9a-f]{960})', raw):
        y = int(y)
        if y in result:
            raise RuntimeError(f'duplicate captured row {y}')
        result[y] = bytes.fromhex(hex_data.decode('ascii'))
    return result


def full_frame(captured):
    if set(captured) != set(range(135)):
        raise RuntimeError(f'incomplete frame: {len(captured)} rows')
    return b''.join(captured[y] for y in range(135))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', default='COM3')
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    if args.out.exists() and any(args.out.iterdir()):
        parser.error('--out must be a new or empty directory')
    args.out.mkdir(parents=True, exist_ok=True)

    events = bytearray()
    with serial.Serial(port=None, baudrate=115200, timeout=0.1) as port:
        port.dtr = False
        port.rts = False
        port.port = args.port
        port.open()

        def read_until(marker, seconds=35):
            raw = bytearray()
            deadline = time.monotonic() + seconds
            while marker not in raw and time.monotonic() < deadline:
                raw.extend(port.read(32768))
            events.extend(raw)
            if marker not in raw:
                raise TimeoutError(f'{marker!r}; tail={bytes(raw[-400:])!r}')
            return bytes(raw)

        def capture(command):
            port.write(command)
            raw = read_until(b'CAPTURE_END', 40)
            if b'CAPTURE_BEGIN 240 135' not in raw:
                raise RuntimeError('capture did not start')
            return raw

        try:
            # The music help presenter is full-screen black with static labels;
            # animation underneath it cannot change the expected RGB565 bytes.
            port.write(b'?')
            time.sleep(1)
            port.reset_input_buffer()
            baseline_raw = capture(b's')
            (args.out / 'baseline-serial.log').write_bytes(baseline_raw)
            baseline = full_frame(rows(baseline_raw))
            nonzero_bytes = sum(byte != 0 for byte in baseline)
            if not nonzero_bytes:
                raise RuntimeError('baseline is completely black; music help is not visible')
            (args.out / 'baseline.rgb565').write_bytes(baseline)

            fault_raw = capture(b'^')
            (args.out / 'fault-serial.log').write_bytes(fault_raw)
            failed = fault_raw.find(b'KSN_P5_REPAIR: INJECT_FAIL y=24 after=3')
            repaired = fault_raw.find(b'KSN_P5_REPAIR: REPAIR_OK bands=17 bytes=64800')
            if failed < 0 or repaired < failed:
                raise RuntimeError('expected injected failure and complete repair')
            first = rows(fault_raw[:failed])
            if set(first) != set(range(24)):
                raise RuntimeError(f'expected first 24 rows before failure: {len(first)}')
            repair = full_frame(rows(fault_raw[failed:repaired]))
            (args.out / 'repair.rgb565').write_bytes(repair)
            differences = sum(baseline[i:i + 2] != repair[i:i + 2]
                              for i in range(0, 64800, 2))
            if differences:
                raise RuntimeError(f'repair differs from baseline at {differences} pixels')

            after_raw = capture(b's')
            (args.out / 'after-serial.log').write_bytes(after_raw)
            after = full_frame(rows(after_raw))
            (args.out / 'after.rgb565').write_bytes(after)
            if after != baseline:
                raise RuntimeError('redraw after repair differs from baseline')
            summary = {
                'pixels': 32400,
                'baseline_nonzero_bytes': nonzero_bytes,
                'baseline_sha256': hashlib.sha256(baseline).hexdigest(),
                'repair_sha256': hashlib.sha256(repair).hexdigest(),
                'after_sha256': hashlib.sha256(after).hexdigest(),
                'failed_bands_sent': 3,
                'repair_bands_sent': 17,
                'difference_pixels': differences,
            }
            (args.out / 'summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
            print(json.dumps(summary, indent=2), flush=True)
        finally:
            (args.out / 'events.log').write_bytes(events)


if __name__ == '__main__':
    main()
