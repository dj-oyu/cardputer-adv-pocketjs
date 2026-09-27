"""Same-image P2 dirty-node/full-scan ABBA, without serial pixel capture."""
import argparse
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
    args = parser.parse_args()
    if args.updates <= 0 or args.updates % 30:
        parser.error('--updates must be a positive multiple of 30')
    if args.out.exists() and any(args.out.iterdir()):
        parser.error('--out must be new or empty')
    args.out.mkdir(parents=True, exist_ok=True)
    port = serial.Serial(port=None, baudrate=115200, timeout=0.02)
    port.dtr = False
    port.rts = False
    port.port = args.port
    results = []
    fullscan = False  # A freshly flashed/reset diagnostic image starts dirty.
    with port:
        def until(marker, seconds, log, start=None):
            if start is None:
                start = len(log)
            deadline = time.monotonic() + seconds
            while marker not in log[start:] and time.monotonic() < deadline:
                log.extend(port.read(32768))
            if marker not in log[start:]:
                raise TimeoutError(f'{marker!r}; tail={bytes(log[-400:])!r}')

        for arm, want_fullscan in enumerate((False, True, True, False), 1):
            name = ('fullscan' if want_fullscan else 'dirty') + f'-{arm}'
            log = bytearray()
            if fullscan != want_fullscan:
                port.write(b'g')
                until(f'KSN_P2: FULLSCAN {int(want_fullscan)}'.encode(), 4, log)
                fullscan = want_fullscan
            port.write(b'q')
            until(b'HOME_READY', 12, log)
            port.write(b'l')
            until(b'KSN_DIRTY24 READY slots=24 nodes=24 stage=0', 12, log)
            for stage in range(1, 4):
                port.write(b'e')
                until(f'KSN_DIRTY24 STAGE {stage}'.encode(), 8, log)
                time.sleep(0.1)
            for _ in range(args.updates):
                port.write(b'e')
                time.sleep(0.055)
                log.extend(port.read(32768))
            last = 3 + args.updates
            until(f'KSN_DIRTY24 STAGE {last}'.encode(), 12, log, start=0)
            time.sleep(0.2)
            port.write(b'q')
            until(b'APP_STOPPED', 12, log)
            (args.out / f'{name}.log').write_bytes(log)
            if b'APP_FAILED' in log or b'panic' in log.lower():
                raise RuntimeError(f'{name}: app failure or panic')
            metrics = {}
            text = log.decode(errors='replace')
            for metric in ('app_turn', 'app_render', 'app_send'):
                pattern = rf'KSN_P0: S session=app metric={metric} seen=(\d+).*?p99=(\d+) max=(\d+) over12=(\d+)'
                match = re.search(pattern, text)
                if not match:
                    raise RuntimeError(f'{name}: missing {metric}')
                metrics[metric] = dict(zip(('seen', 'p99', 'max', 'over12'),
                                           map(int, match.groups())))
            transfer = re.search(r'KSN_P0: T session=app frames=(\d+) lcd_bytes=(\d+) bands=(\d+)', text)
            heap = re.search(r'KSN_P0: M session=app free=(\d+) min=(\d+) largest=(\d+) stack_free=(\d+)', text)
            if not transfer or not heap:
                raise RuntimeError(f'{name}: missing transfer/heap report')
            result = {'arm': name, 'fullscan': want_fullscan,
                      'updates': args.updates, 'metrics': metrics,
                      'transfer': dict(zip(('frames', 'lcd_bytes', 'bands'),
                                           map(int, transfer.groups()))),
                      'memory': dict(zip(('free', 'min', 'largest', 'stack_free'),
                                         map(int, heap.groups())))}
            results.append(result)
            print(json.dumps(result), flush=True)
    (args.out / 'summary.json').write_text(json.dumps(results, indent=2),
                                           encoding='utf-8')


if __name__ == '__main__':
    main()
