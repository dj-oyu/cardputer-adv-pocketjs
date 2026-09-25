"""Capture the known-duration music bar in source or direct diagnostic mode."""
import argparse
from pathlib import Path
import re
import time

import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--mode', choices=('source', 'direct'), required=True)
    parser.add_argument('--reference', type=Path,
                        help='previous rgb565 capture to compare pixel-for-pixel')
    parser.add_argument('--paused-reference', type=Path,
                        help='previous paused rgb565 capture to compare')
    parser.add_argument('--timing-only', action='store_true',
                        help='avoid serial frame captures while timing guest wakeup')
    args = parser.parse_args()
    if args.out.exists() and any(args.out.iterdir()):
        parser.error('--out must be new or empty')
    args.out.mkdir(parents=True, exist_ok=True)
    lines = []
    with (args.out / 'serial.log').open('w', encoding='utf-8') as log:
        port = serial.Serial(port=None, baudrate=115200, timeout=0.15)
        port.dtr = False
        port.rts = False
        port.port = args.port
        port.open()
        try:
            def read_line():
                line = port.readline().decode(errors='replace').strip()
                if line:
                    lines.append(line)
                    log.write(line + '\n')
                return line

            def until(marker, seconds):
                deadline = time.monotonic() + seconds
                while time.monotonic() < deadline:
                    line = read_line()
                    if marker in line:
                        log.flush()
                        print('SEEN', line, flush=True)
                        return line
                    if ('KSN_MUSIC_BAR ERROR' in line or 'APP_FAILED' in line or
                            'panic' in line.lower()):
                        raise RuntimeError(line)
                raise TimeoutError(f'{marker}: {lines[-8:]}')

            port.write(b'q')
            until('HOME_READY', 12)
            if args.mode == 'direct':
                port.write(b't')
                until('KSN_MUSIC_AB: mode=direct', 4)
            port.write(b'i')
            until('KSN_MUSIC_BAR CREATED bytes=22576', 30)
            ready = until('KSN_MUSIC_BAR READY', 15)
            match = re.search(r'positionMs=(\d+) durationMs=(\d+)', ready)
            if not match:
                raise RuntimeError('Missing fixed playback position/duration')
            position, duration = map(int, match.groups())
            if position != 1120 or not 1800 <= duration <= 1900:
                raise RuntimeError(f'Unexpected playback position/duration: {ready}')
            if args.timing_only:
                until('KSN_MUSIC_BAR SLEEP_DONE', 25)
                until('KSN_MUSIC_BAR PAUSED positionMs=1120', 6)
                until('KSN_MUSIC_BAR REMOVED', 35)
                port.write(b'q')
                until('APP_STOPPED', 12)
                print('MUSIC_BAR_TIMING_DONE', args.mode, args.out, flush=True)
                return
            def capture(name, reference):
                # Wait for the binding's acknowledged follow-up frame.
                time.sleep(0.6)
                port.write(b's')
                until('CAPTURE_BEGIN 240 135', 12)
                rows = {}
                deadline = time.monotonic() + 25
                while time.monotonic() < deadline:
                    line = read_line()
                    pixel = re.search(r'PIX (\d+) ([0-9a-f]{960})', line)
                    if pixel:
                        rows[int(pixel.group(1))] = bytes.fromhex(pixel.group(2))
                    if 'CAPTURE_END' in line:
                        break
                if set(rows) != set(range(135)):
                    raise RuntimeError(f'Incomplete {name} capture: {len(rows)} rows')
                pixels = b''.join(rows[y] for y in range(135))
                (args.out / f'{name}.rgb565').write_bytes(pixels)
                if reference:
                    old = reference.read_bytes()
                    if len(old) != len(pixels):
                        raise RuntimeError('Reference has the wrong frame size')
                    for index in range(0, len(pixels), 2):
                        if pixels[index:index + 2] != old[index:index + 2]:
                            x = (index // 2) % 240
                            y = (index // 2) // 240
                            raise RuntimeError(
                                f'{name} source/direct pixel mismatch at {x},{y}')
                    print(f'{name.upper()}_PARITY PASS 32400 pixels', flush=True)
                return pixels

            pixels = capture('frame', args.reference)
            fill = min(216, position * 216 // duration)
            def at(x, y):
                offset = (y * 240 + x) * 2
                return int.from_bytes(pixels[offset:offset + 2], 'big')
            filled = ((0x78 >> 3) << 11) | ((0xc8 >> 2) << 5) | (0xff >> 3)
            empty = ((0x18 >> 3) << 11) | ((0x26 >> 2) << 5) | (0x36 >> 3)
            for x in range(12, 228):
                expected = filled if x < 12 + fill else empty
                if at(x, 109) != expected or at(x, 110) != expected:
                    raise RuntimeError(f'Bar pixel mismatch at x={x}: '
                                       f'{at(x, 109):04x}/{at(x, 110):04x} '
                                       f'expected {expected:04x}')
            print(f'BAR_PIXELS PASS fill={fill}/216 duration={duration}', flush=True)
            if not any('KSN_MUSIC_BAR PAUSED' in line for line in lines):
                until('KSN_MUSIC_BAR PAUSED positionMs=1120', 25)
            slept = next((line for line in lines
                          if 'KSN_MUSIC_BAR SLEEP_DONE' in line), None)
            match = re.search(r'elapsedMs=([\d.]+) state=(\w+)', slept or '')
            if not match or float(match.group(1)) > 500 or match.group(2) != 'playing':
                raise RuntimeError(f'Guest sleep was delayed by playback: {slept}')
            print(f'GUEST_WAKE PASS elapsedMs={match.group(1)}', flush=True)
            paused = capture('paused', args.paused_reference)
            for x in range(12, 228):
                offset = (109 * 240 + x) * 2
                expected = filled if x < 12 + fill else empty
                if int.from_bytes(paused[offset:offset + 2], 'big') != expected:
                    raise RuntimeError(f'Paused bar pixel mismatch at x={x}')
            print(f'PAUSED_BAR_PIXELS PASS fill={fill}/216', flush=True)
            until('KSN_MUSIC_BAR REMOVED', 35)
            port.write(b'q')
            until('APP_STOPPED', 12)
        finally:
            port.close()
    print('MUSIC_BAR_DONE', args.mode, args.out, flush=True)


if __name__ == '__main__':
    main()
