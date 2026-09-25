"""P4 foreground 23-Japanese-text device trial; does not write flash or SD."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import time

import serial


def frame_from(raw):
    rows = {int(y): bytes.fromhex(pixels.decode('ascii'))
            for y, pixels in re.findall(rb'PIX (\d+) ([0-9a-f]{960})', raw)}
    if set(rows) != set(range(135)):
        raise RuntimeError(f'incomplete frame: {len(rows)}/135 rows')
    return b''.join(rows[y] for y in range(135))


def parse_metric(text, name):
    match = re.search(rf'KSN_P0: S session=app metric={name} seen=(\d+).*?'
                      r'p95=(\d+) p99=(\d+) max=(\d+) over12=(\d+)', text)
    if not match:
        raise RuntimeError(f'missing {name} histogram')
    return dict(zip(('seen', 'p95', 'p99', 'max', 'over12'),
                    map(int, match.groups())))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', default='COM3')
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--updates', type=int, default=180)
    parser.add_argument('--pixels', action='store_true')
    parser.add_argument('--lowheap', action='store_true')
    args = parser.parse_args()
    if args.updates < 4 or args.updates > 300:
        parser.error('--updates must be 4..300')
    if args.out.exists() and any(args.out.iterdir()):
        parser.error('--out must be a new or empty directory')
    args.out.mkdir(parents=True, exist_ok=True)
    log = bytearray()
    lowheap_on = False
    port = serial.Serial(port=None, baudrate=115200, timeout=0.02)
    port.dtr = False
    port.rts = False
    port.port = args.port
    with port:
        def until(marker, seconds=15, start=None):
            if start is None:
                start = len(log)
            deadline = time.monotonic() + seconds
            while marker not in log[start:] and time.monotonic() < deadline:
                log.extend(port.read(32768))
            if marker not in log[start:]:
                raise TimeoutError(f'{marker!r}; tail={bytes(log[-500:])!r}')
            return bytes(log[start:])

        def capture(name):
            time.sleep(0.3)
            start = len(log)
            port.write(b's')
            raw = until(b'CAPTURE_END', 35, start)
            frame = frame_from(raw)
            (args.out / f'{name}.rgb565').write_bytes(frame)
            return frame

        try:
            port.write(b'q')
            until(b'HOME_READY', 12)
            if args.lowheap:
                port.write(b'H')
                until(b'KSN_P5_LOWHEAP: ON bytes=16384', 5)
                lowheap_on = True
            port.write(b'T')
            until(b'KSN_TEXT23 READY slots=24 text=23 visible=22', 15)
            until(b'KASANE_FRAME_PRESENTED', 10, 0)
            frames = {'initial': capture('initial')} if args.pixels else {}
            for stage in range(1, args.updates + 1):
                start = len(log)
                port.write(b'e')
                marker = (b'KSN_TEXT23 PREFLIGHT_REJECT stage=3' if stage == 3
                          else f'KSN_TEXT23 STAGE {stage}'.encode())
                if stage <= 4 or stage % 30 == 0:
                    until(marker, 12, start)
                    print('STAGE', stage, flush=True)
                else:
                    time.sleep(0.055)
                    log.extend(port.read(32768))
                if args.pixels and stage <= 4:
                    frames[f'stage{stage}'] = capture(f'stage{stage}')
            if args.pixels:
                if frames['initial'] == frames['stage1'] or \
                   frames['stage1'] == frames['stage2'] or \
                   frames['stage2'] != frames['stage3'] or \
                   frames['stage3'] == frames['stage4']:
                    raise RuntimeError('text update or rejected preflight pixels differ')
            port.write(b'q')
            until(b'APP_STOPPED', 12)
            if lowheap_on:
                port.write(b'H')
                until(b'KSN_P5_LOWHEAP: OFF', 5)
                lowheap_on = False
            text = log.decode(errors='replace')
            if 'APP_FAILED' in text or 'panic' in text.lower():
                raise RuntimeError('app failure or panic')
            if 'KSN_TEXT23 PREFLIGHT_REJECT stage=3' not in text:
                raise RuntimeError('expected 897-byte rejection missing')
            metric = {name: parse_metric(text, name)
                      for name in ('app_turn', 'app_render', 'app_send')}
            transfer = re.search(r'KSN_P0: T session=app frames=(\d+) lcd_bytes=(\d+) bands=(\d+)', text)
            memory = re.search(r'KSN_P0: M session=app free=(\d+) min=(\d+) largest=(\d+) stack_free=(\d+)', text)
            if not transfer or not memory:
                raise RuntimeError('missing transfer or memory report')
            summary = {
                'updates': args.updates, 'lowheap': args.lowheap,
                'pixels': args.pixels, 'metrics': metric,
                'transfer': dict(zip(('frames', 'lcd_bytes', 'bands'),
                                     map(int, transfer.groups()))),
                'memory': dict(zip(('free', 'min', 'largest', 'stack_free'),
                                   map(int, memory.groups()))),
                'frame_sha256': {name: hashlib.sha256(frame).hexdigest()
                                 for name, frame in frames.items()},
            }
            (args.out / 'summary.json').write_text(
                json.dumps(summary, indent=2), encoding='utf-8')
            print(json.dumps(summary, indent=2), flush=True)
        finally:
            port.write(b'q')
            time.sleep(0.2)
            log.extend(port.read(32768))
            if lowheap_on:
                port.write(b'H')
                time.sleep(0.3)
                log.extend(port.read(32768))
            (args.out / 'serial.log').write_bytes(log)


if __name__ == '__main__':
    main()
