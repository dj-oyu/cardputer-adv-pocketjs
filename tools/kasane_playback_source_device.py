"""Gate the generic current-player source on Cardputer without changing SD."""
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
                        help='check ready/playing/paused/resumed/closed LCD pixels')
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

            def until(marker, seconds):
                deadline = time.monotonic() + seconds
                while time.monotonic() < deadline:
                    line = read_line()
                    if marker in line:
                        print('SEEN', line, flush=True)
                        return line
                    if ('APP_FAILED' in line or 'KSN_PLAYBACK_SOURCE ERROR' in line or
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
            port.write(b'h')
            until('KSN_PLAYBACK_SOURCE BOUND', 15)
            until('pocket.sd: PICK 2 folders', 15)
            port.write(b'de')
            until('pocket.sd: GRANTED music', 15)
            until('KSN_PLAYBACK_SOURCE OPEN', 15)
            frames = {}
            if args.capture:
                time.sleep(0.15)
                frames['ready'] = capture('ready')
            until('KSN_PLAYBACK_SOURCE PLAY', 10)
            if args.capture:
                time.sleep(0.15)
                frames['playing'] = capture('playing')
            until('KSN_PLAYBACK_SOURCE PAUSED', 15)
            if args.capture:
                time.sleep(0.15)
                frames['paused'] = capture('paused')
            until('KSN_PLAYBACK_SOURCE RESUMED', 10)
            if args.capture:
                time.sleep(0.15)
                frames['resumed'] = capture('resumed')
            until('KSN_PLAYBACK_SOURCE CLOSED', 25)
            if args.capture:
                time.sleep(0.15)
                frames['closed'] = capture('closed')
            port.write(b'q')
            until('KSN_PLAYBACK_SOURCE: STOP', 12)
            until('APP_STOPPED', 12)

            def last_match(pattern):
                for line in reversed(lines):
                    found = re.search(pattern, line)
                    if found:
                        return found
                return None

            pub = last_match(r'KSN_PLAYBACK_SOURCE: STOP published=(\d+) '
                             r'valid=(\d+) skipped=(\d+) max_position_ms=(\d+)')
            final = last_match(r'KSN_PLAYBACK_SOURCE FINAL positionMs=(\d+) '
                               r'underruns=(\d+)')
            decoder = last_match(r'MP3DEC packets=.*faults=(\d+)')
            text_pixels = {}
            text_regions = {}
            if args.capture:
                for name, data in frames.items():
                    background = data[:2]
                    region = [data[2 * (y * 240 + x):2 * (y * 240 + x) + 2]
                              for y in range(4, 16) for x in range(4, 92)]
                    text_regions[name] = region
                    text_pixels[name] = sum(pixel != background for pixel in region)
            summary = {
                'capture': args.capture,
                'published': int(pub.group(1)) if pub else None,
                'valid_published': int(pub.group(2)) if pub else None,
                'skipped': int(pub.group(3)) if pub else None,
                'max_position_ms': int(pub.group(4)) if pub else None,
                'final_position_ms': int(final.group(1)) if final else None,
                'underruns': int(final.group(2)) if final else None,
                'decoder_faults': int(decoder.group(1)) if decoder else None,
                'text_pixels': text_pixels,
                'errors': [line for line in lines if 'KSN_PLAYBACK_SOURCE ERROR' in line or
                           'IO ERROR' in line or 'STREAM STARVED' in line or
                           'APP_FAILED' in line or 'panic' in line.lower()],
            }
            (args.out / 'summary.json').write_text(json.dumps(summary, indent=2),
                                                   encoding='utf-8')
            if (pub is None or final is None or decoder is None or
                    summary['valid_published'] < 15 or summary['skipped'] != 0 or
                    summary['max_position_ms'] < 18000 or
                    summary['final_position_ms'] < 18000 or
                    summary['underruns'] != 0 or summary['decoder_faults'] != 0 or
                    summary['errors'] or
                    (args.capture and
                     (text_pixels.get('ready') != 0 or
                      text_pixels.get('playing', 0) == 0 or
                      text_pixels.get('paused') != 0 or
                      text_pixels.get('resumed', 0) == 0 or
                      text_pixels.get('closed') != 0 or
                      text_regions['playing'] != text_regions['resumed']))):
                raise RuntimeError(f'playback source gate: {summary}')
            print('PLAYBACK_SOURCE PASS', summary, flush=True)
        finally:
            port.close()


if __name__ == '__main__':
    main()
