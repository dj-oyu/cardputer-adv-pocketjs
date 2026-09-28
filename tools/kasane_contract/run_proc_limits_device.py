"""Run the procedural limits probe on a KASANE_PROC_LIMITS_PROBE=ON image.

'{' runs the native measurements at HOME (VM step, points, band render,
registration stack). '}' starts apps/kasane/proc_limits_probe.js: plan
lifecycle and heap, limit checks, the limits scene (captured over USB and
compared with the host reference from run_proc_limits_scene.py), then the
per-frame VM step scaling. The whole serial log is saved; KSN_LIMITS lines are
echoed. Exit status is 0 only if the capture matches and every JS CHECK held.
"""
import argparse
from pathlib import Path
import re
import time

import serial

W, H = 240, 135
PIX = re.compile(rb"^PIX (\d+) ([0-9a-f]{960})\r?$", re.M)


def read_until(port, log, predicate, seconds):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        log.extend(port.read(32768))
        if predicate(log):
            return True
    return False


def capture_rgb565(chunk: bytes):
    rows = {}
    for match in PIX.finditer(chunk):
        rows[int(match[1])] = bytes.fromhex(match[2].decode('ascii'))
    if set(rows) != set(range(H)):
        return None
    # PIX prints big-endian hex per pixel; the host file is little-endian.
    return b''.join(bytes(v for i in range(0, W * 2, 2) for v in (rows[y][i + 1], rows[y][i]))
                    for y in range(H))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--port', default='COM3')
    ap.add_argument('--out', type=Path, required=True, help='serial log path')
    ap.add_argument('--expected', type=Path,
                    default=Path(__file__).resolve().parents[2] / '.cache/kasane_proc_limits/expected.rgb565')
    ap.add_argument('--skip-native', action='store_true')
    ap.add_argument('--skip-js', action='store_true')
    args = ap.parse_args()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    port = serial.Serial(port=None, baudrate=115200, timeout=0.1)
    port.dtr = False
    port.rts = False
    port.port = args.port
    log = bytearray()
    status = 0
    try:
        with port:
            if not read_until(port, log, lambda b: b'HOME_READY' in b, 12):
                port.write(b'q')
                if not read_until(port, log, lambda b: b'HOME_READY' in b, 12):
                    print('HOME_READY missing')
                    return 2
            if not args.skip_native:
                start = len(log)
                port.write(b'{')
                if not read_until(port, log, lambda b: b'KSN_LIMITS: END' in b[start:] and
                                  b'HOME_READY' in b[start:], 180):
                    print('native probe did not finish')
                    return 2
            if not args.skip_js:
                start = len(log)
                port.write(b'}')
                if not read_until(port, log, lambda b: b'LIMITS_SCENE_READY' in b[start:], 120):
                    print('LIMITS_SCENE_READY missing')
                    return 2
                port.write(b's')
                cap = len(log)
                if not read_until(port, log, lambda b: b'CAPTURE_END' in b[cap:] and
                                  len(PIX.findall(bytes(b[cap:]))) >= H, 30):
                    print('capture incomplete')
                    return 2
                image = capture_rgb565(bytes(log[cap:]))
                expected = args.expected.read_bytes()
                if image is None:
                    print('capture rows missing')
                    status = 2
                else:
                    diff = sum(1 for i in range(0, W * H * 2, 2) if image[i:i + 2] != expected[i:i + 2])
                    print(f'CAPTURE vs host reference: differing pixels={diff}')
                    (args.out.parent / 'capture.rgb565').write_bytes(image)
                    status |= 0 if diff == 0 else 2
                if not read_until(port, log, lambda b: b'LIMITS_DONE' in b[start:] or
                                  b'RUNAWAY' in b[start:], 180):
                    print('LIMITS_DONE missing')
                    status = 2
                done = re.search(rb'LIMITS_DONE fails=(\d+)', bytes(log[start:]))
                if not done or int(done[1]):
                    status = 2
                read_until(port, log, lambda b: b'JS PARK' in b[start:], 10)
                # Frames of 1..9 x 25 ms draws, then a steady 100 ms frame that
                # Back has to interrupt: how long the leave turn takes to reach
                # home while every frame spans several turns.
                if not read_until(port, log, lambda b: b'HEAVY_START' in b[start:] or
                                  b'RUNAWAY' in b[start:], 60):
                    print('HEAVY_START missing')
                    status = 2
                time.sleep(1.5)
                back = len(log)
                sent = time.monotonic()
                port.write(b'q')
                if not read_until(port, log, lambda b: b'HOME_READY' in b[back:], 20):
                    print('HOME_READY missing after JS probe')
                    status = 2
                else:
                    print(f'BACK_TO_HOME_MS {(time.monotonic() - sent) * 1000:.0f} (host clock, includes USB)')
    finally:
        args.out.write_bytes(log)
        for line in bytes(log).splitlines():
            if b'KSN_LIMITS' in line or b'RUNAWAY' in line or b'E (' in line:
                print(line.decode('utf-8', 'replace'))
        print(f'Saved {len(log)} bytes to {args.out}')
    return status


if __name__ == '__main__':
    raise SystemExit(main())
