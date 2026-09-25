"""Device check: pending 24-slot update survives partial LCD IO failure."""
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
            time.sleep(0.35)

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
            until(b'KSN_DIRTY24 COMPOSITE_DIRECT', 8)
            direct = capture('direct')
            launch()
            port.write(b'@')
            until(b'KSN_P2: ARMED_PATCH after=3', 8)
            port.write(b']')
            failed_raw = until(b'CAPTURE_END', 35)
            if b'KSN_DIRTY24 COMPOSITE_BURST' not in failed_raw:
                raise RuntimeError('the two-set burst did not run')
            repaired = frame_from(failed_raw)
            (args.out / 'repair.rgb565').write_bytes(repaired)
            final = capture('final')

            set_pattern = rb'KSN_P2_SET revision=(\d+) ticket=(\d+) inflight=([0-9a-f]{8}) pending=([0-9a-f]{8})'
            sets = re.findall(set_pattern, failed_raw)
            if len(sets) != 2:
                raise RuntimeError(f'expected two set traces: {sets!r}')
            first, second = sets
            if not int(first[1]) or first[1] != second[1]:
                raise RuntimeError(f'updates did not share an in-flight ticket: {sets!r}')
            if int(first[2], 16) != 0x155555 or int(first[3], 16) != 0:
                raise RuntimeError(f'first column submission was unexpected: {first!r}')
            if int(second[3], 16) != 0x155557:
                raise RuntimeError(f'latest values lost pending bits: {second!r}')
            rects = re.findall(rb'KSN_P2: SEND_RECT x=(\d+) y=(\d+) cols=(\d+) rows=(\d+)',
                               failed_raw)
            if len(rects) != 4 or b'KSN_P2: SEND_FULL' in failed_raw:
                raise RuntimeError(f'not a four-attempt narrow PATCH: {rects!r}')
            if [(int(x), int(y), int(cols), int(rows)) for x, y, cols, rows in rects] != [
                    (0, 0, 32, 8), (0, 8, 32, 8),
                    (0, 16, 32, 8), (0, 24, 32, 8)]:
                raise RuntimeError(f'unexpected PATCH geometry: {rects!r}')
            if b'KSN_P2: INJECT_FAIL y=24 after=3' not in failed_raw or \
               b'KSN_P2: PARTIAL_FAILED sent=3' not in failed_raw:
                raise RuntimeError('fourth send did not fail as injected')
            match = re.search(rb'KSN_P2: REPAIR_OK bands=(\d+) bytes=(\d+)', failed_raw)
            if not match or (int(match[1]), int(match[2])) != (17, 64800):
                raise RuntimeError('the partial failure was not repaired full-screen')
            if initial == direct:
                raise RuntimeError('direct final values did not change pixels')
            mismatches = sum(direct[i:i + 2] != final[i:i + 2]
                             for i in range(0, len(direct), 2))
            if mismatches:
                raise RuntimeError(f'final frame lost pending update: {mismatches} pixels')
            port.write(b'q')
            until(b'APP_STOPPED', 12)
            if b'APP_FAILED' in log or b'panic' in log.lower():
                raise RuntimeError('app failure or panic')
            summary = {
                'shared_ticket': int(first[1]),
                'first_inflight_mask': int(first[2], 16),
                'second_pending_mask': int(second[3], 16),
                'successful_partial_sends': 3,
                'injected_send': 4,
                'repair_bands': int(match[1]),
                'repair_bytes': int(match[2]),
                'final_pixel_mismatches': mismatches,
                'sha256_direct': hashlib.sha256(direct).hexdigest(),
                'sha256_repair': hashlib.sha256(repaired).hexdigest(),
                'sha256_final': hashlib.sha256(final).hexdigest(),
            }
            (args.out / 'summary.json').write_text(json.dumps(summary, indent=2),
                                                   encoding='utf-8')
            print(json.dumps(summary, indent=2), flush=True)
        finally:
            (args.out / 'serial.log').write_bytes(log)


if __name__ == '__main__':
    main()
