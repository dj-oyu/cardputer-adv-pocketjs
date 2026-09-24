"""Exercise diagnostic v: audio-task producer -> pool -> mounted text slot."""
import argparse
import json
from pathlib import Path
import re
import time

import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--capture', action='store_true',
                        help='capture live and ended LCD frames (extra serial load)')
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
                    log.flush()
                return line

            def until(marker, seconds, seen_ok=False):
                if seen_ok:
                    for prior in lines:
                        if marker in prior:
                            return prior
                deadline = time.monotonic() + seconds
                while time.monotonic() < deadline:
                    line = read_line()
                    if marker in line:
                        print('SEEN', line, flush=True)
                        return line
                    if ('APP_FAILED' in line or 'KSN_OUTPUT_SOURCE ERROR' in line or
                            'panic' in line.lower()):
                        raise RuntimeError(line)
                raise TimeoutError(f'{marker}: {lines[-8:]}')

            def capture(name):
                port.write(b's')
                until('CAPTURE_BEGIN', 10)
                rows = {}
                deadline = time.monotonic() + 20
                while time.monotonic() < deadline:
                    line = read_line()
                    match = re.search(r'PIX (\d+) ([0-9a-f]{960})', line)
                    if match:
                        rows[int(match.group(1))] = bytes.fromhex(match.group(2))
                    if 'CAPTURE_END' in line:
                        break
                if len(rows) != 135:
                    raise RuntimeError(f'{name}: incomplete LCD capture ({len(rows)} rows)')
                pixels = b''.join(rows[y] for y in range(135))
                (args.out / f'{name}.rgb565').write_bytes(pixels)
                return pixels

            port.write(b'q')
            until('HOME_READY', 12)
            port.write(b'v')
            until('KSN_OUTPUT_SOURCE BOUND', 15)
            until('pocket.sd: PICK 2 folders', 15)
            port.write(b'de')
            until('pocket.sd: GRANTED music', 15)
            until('KSN_OUTPUT_SOURCE OPEN', 15)
            until('KSN_OUTPUT_SOURCE PLAY', 15)
            live = None
            if args.capture:
                time.sleep(1)
                live = capture('live')
            until('KSN_OUTPUT_SOURCE CLOSED', 65, seen_ok=True)
            time.sleep(0.5)
            ended = capture('ended') if args.capture else None
            port.write(b'q')
            stop = until('KSN_OUTPUT_SOURCE: STOP', 12)
            until('APP_STOPPED', 12)
            match = re.search(r'published=(\d+) skipped=(\d+) audio_stopped=(\d+)', stop)
            if not match:
                raise RuntimeError(f'unexpected source stop line: {stop}')
            changed = outside = None
            if live is not None:
                changed = outside = 0
                for y in range(135):
                    for x in range(240):
                        at = 2 * (y * 240 + x)
                        if live[at:at + 2] != ended[at:at + 2]:
                            changed += 1
                            if x >= 96 or y >= 24:
                                outside += 1
            errors = [line for line in lines if 'APP_FAILED' in line or
                      'KSN_OUTPUT_SOURCE ERROR' in line or 'panic' in line.lower() or
                      'STREAM STARVED' in line or 'MP3 stop source_fault=1' in line]
            p0 = [line for line in lines if 'KSN_P0: A session=' in line]
            final = [line for line in lines if 'KSN_OUTPUT_SOURCE FINAL' in line]
            final_match = re.search(r'underruns=(\d+)', final[-1]) if final else None
            summary = {'binary_probe': 'v', 'capture': args.capture,
                       'published': int(match.group(1)),
                       'skipped': int(match.group(2)),
                       'audio_stopped': int(match.group(3)),
                       'changed_pixels': changed,
                       'outside_source_pixels': outside,
                       'audio_log': p0, 'final_log': final,
                       'final_underruns': int(final_match.group(1)) if final_match else None,
                       'errors': errors}
            (args.out / 'summary.json').write_text(json.dumps(summary, indent=2),
                                                   encoding='utf-8')
            if (summary['published'] < 3 or summary['audio_stopped'] != 1 or
                    summary['final_underruns'] != 0 or errors or
                    (changed is not None and (changed == 0 or outside != 0))):
                raise RuntimeError(f'output-source gate: {summary}')
            print('OUTPUT_SOURCE PASS', summary, flush=True)
        finally:
            port.close()


if __name__ == '__main__':
    main()
