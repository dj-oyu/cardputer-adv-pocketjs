"""24-slot device check: two sets while the first ticket is submitted."""
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

        def capture(name):
            time.sleep(0.35)
            port.write(b's')
            frame = frame_from(until(b'CAPTURE_END', 35))
            (args.out / f'{name}.rgb565').write_bytes(frame)
            return frame

        try:
            launch()
            initial = capture('initial')
            port.write(b'!')
            direct_log = until(b'KSN_DIRTY24 DIRECT', 8)
            direct = capture('direct')
            launch()
            port.write(b'{')
            burst_log = until(b'KSN_DIRTY24 BURST', 8)
            burst = capture('burst')

            pattern = rb'KSN_P2_SET revision=(\d+) ticket=(\d+) inflight=([0-9a-f]{8}) pending=([0-9a-f]{8})'
            direct_sets = re.findall(pattern, direct_log)
            burst_sets = re.findall(pattern, burst_log)
            if len(direct_sets) != 1 or len(burst_sets) != 2:
                raise RuntimeError(f'unexpected set traces: direct={direct_sets!r} '
                                   f'burst={burst_sets!r}')
            first, second = burst_sets
            first_ticket, second_ticket = int(first[1]), int(second[1])
            if not first_ticket or first_ticket != second_ticket:
                raise RuntimeError(f'second set was not queued behind first: {burst_sets!r}')
            if int(first[2], 16) != 1 or int(first[3], 16) != 0:
                raise RuntimeError(f'first set did not submit v0: {first!r}')
            if int(second[3], 16) != 0x5:
                raise RuntimeError(f'second set lost pending v0/v2: {second!r}')
            if initial == direct:
                raise RuntimeError('direct final values did not change pixels')
            mismatches = sum(direct[i:i + 2] != burst[i:i + 2]
                             for i in range(0, len(direct), 2))
            if mismatches:
                raise RuntimeError(f'burst final frame differs by {mismatches} pixels')
            port.write(b'q')
            until(b'APP_STOPPED', 12)
            if b'APP_FAILED' in log or b'panic' in log.lower():
                raise RuntimeError('app failure or panic')
            summary = {
                'first_ticket': first_ticket,
                'second_ticket': second_ticket,
                'first_inflight_mask': int(first[2], 16),
                'second_pending_mask': int(second[3], 16),
                'direct_vs_burst_pixel_mismatches': mismatches,
                'sha256_direct': hashlib.sha256(direct).hexdigest(),
                'sha256_burst': hashlib.sha256(burst).hexdigest(),
            }
            (args.out / 'summary.json').write_text(json.dumps(summary, indent=2),
                                                   encoding='utf-8')
            print(json.dumps(summary, indent=2), flush=True)
        finally:
            (args.out / 'serial.log').write_bytes(log)


if __name__ == '__main__':
    main()
