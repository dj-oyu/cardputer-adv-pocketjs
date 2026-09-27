"""Same-image Kasane binary-text PIE ABBA, followed by OFF/ON pixel captures.

Requires KASANE_P0_PROBE and KASANE_TEXT_PIE_DEVICE_PROBE. The USB 'K' switch
is sent only at HOME_READY, between measured app sessions.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import time

import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', default='COM3')
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--updates', type=int, default=180)
    parser.add_argument('--workload', choices=('hello', 'text'), default='hello')
    args = parser.parse_args()
    if not 30 <= args.updates <= 300:
        parser.error('--updates must be 30..300')
    if args.out.exists() and any(args.out.iterdir()):
        parser.error('--out must be new or empty')
    args.out.mkdir(parents=True, exist_ok=True)

    port = serial.Serial(port=None, baudrate=115200, timeout=0.1)
    port.dtr = False
    port.rts = False
    port.port = args.port
    enabled = None  # Unknown after an interrupted previous run.
    results = []

    def until(marker, log, seconds=12, start=None):
        if start is None:
            start = len(log)
        deadline = time.monotonic() + seconds
        while marker not in log[start:] and time.monotonic() < deadline:
            log.extend(port.read(32768))
        if marker not in log[start:]:
            raise TimeoutError(f'{marker!r}; tail={bytes(log[-500:])!r}')

    def collect(log, seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            log.extend(port.read(32768))

    def home(log):
        start = len(log)
        port.write(b'q')
        until(b'HOME_READY', log, start=start)

    def select(want, log):
        nonlocal enabled
        if enabled == want:
            return
        marker = f'KSN_PIE: TEXT {int(want)}'.encode()
        for attempt in range(3):
            start = len(log)
            port.write(b'K' if want else b'k')
            try:
                until(marker, log, 2, start)
                break
            except TimeoutError:
                if attempt == 2:
                    raise
        enabled = want

    def launch(log):
        if args.workload == 'text':
            start = len(log)
            port.write(b'?')
            until(b'KASANE_FRAME_PRESENTED', log, start=start)
            if b'TEXT_PIE_READY' not in log[start:]:
                raise RuntimeError('text workload did not initialize')
            return
        start = len(log)
        port.write(b'a')
        until(b'CATEGORY 0', log, start=start)
        start = len(log)
        port.write(b'e')
        until(b'KASANE_FRAME_PRESENTED', log, start=start)

    def update(count, log):
        start = len(log)
        port.write(b'e')
        marker = f'TEXT_PIE_STEP {count}' if args.workload == 'text' else f'HELLO_COUNT {count}'
        until(marker.encode(), log, 5, start)

    def stop(log):
        start = len(log)
        port.write(b'q')
        until(b'APP_STOPPED', log, start=start)
        collect(log, 0.7)

    def parse_metrics(log, name, want):
        report = log.decode(errors='replace')
        if b'APP_FAILED' in log or b'panic' in log.lower():
            raise RuntimeError(f'{name}: app failure or panic')
        metrics = {}
        for metric in ('app_turn', 'app_render', 'app_send'):
            pattern = rf'KSN_P0: S session=app metric={metric} seen=(\d+).*?p95=(\d+) p99=(\d+) max=(\d+) over12=(\d+)'
            match = re.search(pattern, report)
            if not match:
                raise RuntimeError(f'{name}: missing {metric}')
            metrics[metric] = dict(zip(('seen', 'p95', 'p99', 'max', 'over12'),
                                       map(int, match.groups())))
        transfer = re.search(r'KSN_P0: T session=app frames=(\d+) lcd_bytes=(\d+) bands=(\d+)', report)
        memory = re.search(r'KSN_P0: M session=app free=(\d+) min=(\d+) largest=(\d+) stack_free=(\d+)', report)
        if not transfer or not memory:
            raise RuntimeError(f'{name}: missing transfer/heap report')
        return {
            'arm': name, 'text_pie': want, 'workload': args.workload, 'updates': args.updates,
            'metrics': metrics,
            'transfer': dict(zip(('frames', 'lcd_bytes', 'bands'), map(int, transfer.groups()))),
            'memory': dict(zip(('free', 'min', 'largest', 'stack_free'), map(int, memory.groups()))),
        }

    def capture(want):
        log = bytearray()
        home(log)
        select(want, log)
        launch(log)
        update(1, log)
        update(2, log)
        start = len(log)
        port.write(b's')
        until(b'CAPTURE_END', log, 15, start)
        rows = re.findall(rb'PIX (\d+) ([0-9a-f]{960})', bytes(log[start:]))
        if sorted(int(y) for y, _ in rows) != list(range(135)):
            raise RuntimeError(f'capture {want}: expected 135 complete rows, got {len(rows)}')
        pixels = b''.join(bytes.fromhex(row.decode()) for _, row in sorted(rows, key=lambda p: int(p[0])))
        digest = hashlib.sha256(pixels).hexdigest()
        stop(log)
        (args.out / f'capture-{int(want)}.log').write_bytes(log)
        return digest

    try:
        with port:
            for index, want in enumerate((False, True, True, False), 1):
                name = ('on' if want else 'off') + f'-{index}'
                log = bytearray()
                home(log)
                select(want, log)
                launch(log)
                for count in range(1, args.updates + 1):
                    update(count, log)
                stop(log)
                (args.out / f'{name}.log').write_bytes(log)
                result = parse_metrics(log, name, want)
                results.append(result)
                print(json.dumps(result), flush=True)
            hashes = {'off': capture(False), 'on': capture(True)}
            if hashes['off'] != hashes['on']:
                raise RuntimeError(f'pixel mismatch: {hashes}')
            print('PIXELS_MATCH', hashes['off'], flush=True)
    finally:
        (args.out / 'summary.json').write_text(
            json.dumps({'arms': results, 'capture_sha256': locals().get('hashes')}, indent=2),
            encoding='utf-8')


if __name__ == '__main__':
    main()
