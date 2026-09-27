"""P2 foreground 24-slot/page/visibility pixel trial (diagnostic image only)."""
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


def pixel(frame, x, y):
    at = 2 * (y * 240 + x)
    return int.from_bytes(frame[at:at + 2], 'big')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', default='COM3')
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--toggle-fullscan', action='store_true',
                        help='switch the same diagnostic image to full-scan before launch')
    parser.add_argument('--reference-dir', type=Path,
                        help='compare all four frames against a prior same-image run')
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
        def until(marker, seconds=20):
            raw = bytearray()
            deadline = time.monotonic() + seconds
            while marker not in raw and time.monotonic() < deadline:
                raw.extend(port.read(32768))
            log.extend(raw)
            if marker not in raw:
                raise TimeoutError(f'{marker!r}; tail={bytes(raw[-400:])!r}')
            return bytes(raw)

        def capture(name):
            time.sleep(0.4)
            port.reset_input_buffer()
            port.write(b's')
            raw = until(b'CAPTURE_END', 35)
            if b'CAPTURE_BEGIN 240 135' not in raw:
                raise RuntimeError(f'{name}: capture did not start')
            frame = frame_from(raw)
            (args.out / f'{name}.rgb565').write_bytes(frame)
            if args.reference_dir:
                reference = (args.reference_dir / f'{name}.rgb565').read_bytes()
                if len(reference) != len(frame):
                    raise RuntimeError(f'{name}: reference frame size mismatch')
                if frame != reference:
                    at = next(i for i in range(0, len(frame), 2)
                              if frame[i:i + 2] != reference[i:i + 2]) // 2
                    raise RuntimeError(f'{name}: source/full-scan pixel mismatch '
                                       f'at {at % 240},{at // 240}')
            return frame

        try:
            if args.toggle_fullscan:
                port.write(b'g')
                until(b'KSN_P2: FULLSCAN 1', 4)
            port.write(b'q')
            until(b'HOME_READY', 12)
            port.write(b'l')
            until(b'KSN_DIRTY24 READY slots=24 nodes=24 stage=0', 15)
            initial = capture('initial')
            for stage in range(1, 4):
                port.write(b'e')
                until(f'KSN_DIRTY24 STAGE {stage}'.encode('ascii'), 8)
                frame = capture(f'stage{stage}')
                if stage == 1:
                    one = frame
                elif stage == 2:
                    all_changed = frame
                else:
                    restored = frame
            changed_one = sum(initial[i:i + 2] != one[i:i + 2]
                              for i in range(0, len(initial), 2))
            changed_all = sum(initial[i:i + 2] != all_changed[i:i + 2]
                              for i in range(0, len(initial), 2))
            residual = sum(initial[i:i + 2] != restored[i:i + 2]
                           for i in range(0, len(initial), 2))
            if not changed_one or not changed_all or residual:
                raise RuntimeError(f'pixel changes one={changed_one} all={changed_all} '
                                   f'residual={residual}')
            changed_one_locations = [i // 2 for i in range(0, len(initial), 2)
                                     if initial[i:i + 2] != one[i:i + 2]]
            if any(not (4 <= at % 240 < 115 and 2 <= at // 240 < 13)
                   for at in changed_one_locations):
                raise RuntimeError('one-slot update changed pixels outside node 0')
            if pixel(initial, 118, 50) == pixel(all_changed, 118, 50):
                raise RuntimeError('visible slot did not reveal separator')
            if pixel(initial, 4, 133) == pixel(all_changed, 4, 133):
                raise RuntimeError('page slot did not reveal bottom marker')
            port.write(b'q')
            until(b'APP_STOPPED', 12)
            if b'APP_FAILED' in log or b'panic' in log.lower():
                raise RuntimeError('diagnostic reported app failure or panic')
            summary = {'changed_one_pixels': changed_one,
                       'changed_all_pixels': changed_all,
                       'restored_difference_pixels': residual,
                       'reference_pixel_mismatches': 0 if args.reference_dir else None,
                       'sha256_initial': hashlib.sha256(initial).hexdigest(),
                       'sha256_restored': hashlib.sha256(restored).hexdigest()}
            (args.out / 'summary.json').write_text(json.dumps(summary, indent=2),
                                                   encoding='utf-8')
            print(json.dumps(summary, indent=2), flush=True)
        finally:
            (args.out / 'serial.log').write_bytes(log)


if __name__ == '__main__':
    main()
