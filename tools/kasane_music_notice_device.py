"""Capture a SYSTEM notice over the static music help presenter on Cardputer.

Requires HOME OVERLAY=music and KASANE_P5_NOTICE_PROBE=ON. Captures pre-SPI
RGB565 bytes; never flashes or writes the SD card.
"""
import argparse
import hashlib
import json
from pathlib import Path
import time

import serial

from kasane_overlay_repair_pixels import full_frame, rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', default='COM3')
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    if args.out.exists() and any(args.out.iterdir()):
        parser.error('--out must be a new or empty directory')
    args.out.mkdir(parents=True, exist_ok=True)
    events = bytearray()
    port = serial.Serial(port=None, baudrate=115200, timeout=0.1)
    port.dtr = False
    port.rts = False
    port.port = args.port
    with port:
        def read_until(marker, seconds=35):
            raw = bytearray()
            deadline = time.monotonic() + seconds
            while marker not in raw and time.monotonic() < deadline:
                raw.extend(port.read(32768))
            events.extend(raw)
            if marker not in raw:
                raise TimeoutError(f'{marker!r}; tail={bytes(raw[-400:])!r}')
            return bytes(raw)

        def capture(name):
            port.write(b's')
            raw = read_until(b'CAPTURE_END', 40)
            if b'CAPTURE_BEGIN 240 135' not in raw:
                raise RuntimeError(f'{name}: capture did not start')
            frame = full_frame(rows(raw))
            (args.out / f'{name}.rgb565').write_bytes(frame)
            return frame

        try:
            port.write(b'?')
            time.sleep(0.6)
            port.reset_input_buffer()
            baseline = capture('baseline')
            if not any(baseline):
                raise RuntimeError('music help baseline is blank')

            port.write(b'J')
            posted = read_until(b'KSN_P5_NOTICE: POST result=0 id=', 8)
            time.sleep(0.5)
            port.reset_input_buffer()
            notice = capture('notice')

            port.write(b'C')
            cleared = read_until(b'KSN_P5_NOTICE: CLEAR', 8)
            time.sleep(0.5)
            port.reset_input_buffer()
            after = capture('after')

            difference = sum(baseline[i:i + 2] != notice[i:i + 2]
                             for i in range(0, 64800, 2))
            residual = sum(baseline[i:i + 2] != after[i:i + 2]
                           for i in range(0, 64800, 2))
            if not difference:
                raise RuntimeError('SYSTEM notice changed no pixels')
            if residual:
                raise RuntimeError(f'notice removal left {residual} changed pixels')
            if b'panic' in posted.lower() + cleared.lower():
                raise RuntimeError('device reported panic')
            summary = {
                'notice_difference_pixels': difference,
                'after_difference_pixels': residual,
                'baseline_sha256': hashlib.sha256(baseline).hexdigest(),
                'notice_sha256': hashlib.sha256(notice).hexdigest(),
                'after_sha256': hashlib.sha256(after).hexdigest(),
            }
            (args.out / 'summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
            print(json.dumps(summary, indent=2), flush=True)
        finally:
            (args.out / 'events.log').write_bytes(events)


if __name__ == '__main__':
    main()
