"""Compare a clean 24-slot column PATCH with a three-band partial-send repair."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import time

import serial


def frame_from(raw):
    rows = {}
    for y, pixels in re.findall(rb'PIX (\d+) ([0-9a-f]{960})', raw):
        rows[int(y)] = bytes.fromhex(pixels.decode('ascii'))
    if set(rows) != set(range(135)):
        raise RuntimeError(f'incomplete capture: {len(rows)}/135 rows')
    return b''.join(rows[y] for y in range(135))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', default='COM3')
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    if args.out.exists() and any(args.out.iterdir()):
        parser.error('--out must be new or empty')
    args.out.mkdir(parents=True, exist_ok=True)
    log = bytearray()
    port = serial.Serial(port=None, baudrate=115200, timeout=0.1)
    port.dtr = False
    port.rts = False
    port.port = args.port
    with port:
        def until(marker, seconds=25):
            raw = bytearray()
            deadline = time.monotonic() + seconds
            while marker not in raw and time.monotonic() < deadline:
                raw.extend(port.read(32768))
            log.extend(raw)
            if marker not in raw:
                raise TimeoutError(f'{marker!r}; tail={bytes(raw[-500:])!r}')
            return bytes(raw)

        def launch():
            port.write(b'q')
            until(b'HOME_READY', 12)
            port.write(b'l')
            until(b'KSN_DIRTY24 READY slots=24 nodes=24 stage=0', 15)
            for stage in range(1, 4):
                port.write(b'e')
                until(f'KSN_DIRTY24 STAGE {stage}'.encode(), 8)
            time.sleep(0.25)

        try:
            launch()
            port.write(b'}')
            until(b'KSN_DIRTY24 COLUMN', 8)
            time.sleep(0.25)
            port.write(b's')
            clean_raw = until(b'CAPTURE_END', 35)
            clean = frame_from(clean_raw)
            (args.out / 'clean.rgb565').write_bytes(clean)

            launch()
            port.write(b'@')
            until(b'KSN_P2: ARMED_PATCH after=3', 8)
            port.write(b'}')
            repaired_raw = until(b'CAPTURE_END', 35)
            repaired = frame_from(repaired_raw)
            (args.out / 'repaired.rgb565').write_bytes(repaired)

            sends = re.findall(rb'KSN_P2: SEND_RECT x=(\d+) y=(\d+) cols=(\d+) rows=(\d+)',
                               repaired_raw)
            # The probe logs each attempt before p2_fail_this_send: three
            # successful rectangles and the injected fourth attempt.
            if len(sends) != 4 or b'KSN_P2: SEND_FULL' in repaired_raw:
                raise RuntimeError(f'expected four narrow attempts, got {sends!r}')
            if any(int(x) != 0 or int(cols) >= 240 for x, _, cols, _ in sends):
                raise RuntimeError(f'not a left-column partial PATCH: {sends!r}')
            if b'KSN_P2: INJECT_FAIL' not in repaired_raw:
                raise RuntimeError('fourth send was not failed')
            if b'KSN_P2: PARTIAL_FAILED sent=3' not in repaired_raw:
                raise RuntimeError('partial failure was not recorded')
            match = re.search(rb'KSN_P2: REPAIR_OK bands=(\d+) bytes=(\d+)', repaired_raw)
            if not match or (int(match[1]), int(match[2])) != (17, 64800):
                raise RuntimeError(f'repair was not full-frame: {match!r}')
            mismatches = sum(clean[i:i + 2] != repaired[i:i + 2]
                             for i in range(0, len(clean), 2))
            if mismatches:
                raise RuntimeError(f'repaired frame differs by {mismatches} pixels')
            port.write(b'q')
            until(b'APP_STOPPED', 12)
            if b'APP_FAILED' in log or b'panic' in log.lower():
                raise RuntimeError('app failure or panic')
            summary = {
                'partial_rect_attempts': [list(map(int, rect)) for rect in sends],
                'successful_rect_sends': 3,
                'injected_send': 4,
                'repair_bands': int(match[1]),
                'repair_bytes': int(match[2]),
                'repaired_pixel_mismatches': mismatches,
                'sha256_clean': hashlib.sha256(clean).hexdigest(),
                'sha256_repaired': hashlib.sha256(repaired).hexdigest(),
            }
            (args.out / 'summary.json').write_text(json.dumps(summary, indent=2),
                                                   encoding='utf-8')
            print(json.dumps(summary, indent=2), flush=True)
        finally:
            (args.out / 'serial.log').write_bytes(log)


if __name__ == '__main__':
    main()
