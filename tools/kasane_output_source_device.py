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
    parser.add_argument('--probe', choices=('u', 'v'), default='v',
                        help='u=observer off; v=audio output source on')
    parser.add_argument('--capture', action='store_true',
                        help='capture live and ended LCD frames (extra serial load)')
    args = parser.parse_args()
    if args.probe == 'u' and args.capture:
        parser.error('LCD source capture requires --probe v')
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
            port.write(args.probe.encode('ascii'))
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
            stop = until('KSN_OUTPUT_SOURCE: STOP', 12) if args.probe == 'v' else None
            until('APP_STOPPED', 12)
            match = (re.search(r'published=(\d+) skipped=(\d+) audio_stopped=(\d+)', stop)
                     if stop else None)
            if args.probe == 'v' and not match:
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
                      'KSN_OUTPUT_SOURCE STATE error' in line or
                      'STREAM STARVED' in line or 'MP3 stop source_fault=1' in line]
            p0 = [line for line in lines if 'KSN_P0: A session=' in line]
            final = [line for line in lines if 'KSN_OUTPUT_SOURCE FINAL' in line]
            final_match = re.search(r'underruns=(\d+)', final[-1]) if final else None
            decoders = [line for line in lines if 'MP3DEC packets=' in line]
            decoder_match = re.search(r'faults=(\d+)', decoders[-1]) if decoders else None
            metrics = {}
            for line in lines:
                found = re.search(r'KSN_P0: S session=app metric=(\w+).*?p95=(\d+) '
                                  r'p99=(\d+) max=(\d+) over12=(\d+)', line)
                if found:
                    metrics[found.group(1)] = {'p95': int(found.group(2)),
                                               'p99': int(found.group(3)),
                                               'max': int(found.group(4)),
                                               'over12': int(found.group(5))}
            summary = {'binary_probe': args.probe, 'capture': args.capture,
                       'published': int(match.group(1)) if match else None,
                       'skipped': int(match.group(2)) if match else None,
                       'audio_stopped': int(match.group(3)) if match else None,
                       'changed_pixels': changed,
                       'outside_source_pixels': outside,
                       'audio_log': p0, 'final_log': final,
                       'final_underruns': int(final_match.group(1)) if final_match else None,
                       'decoder_faults': int(decoder_match.group(1)) if decoder_match else None,
                       'metrics': metrics, 'errors': errors}
            (args.out / 'summary.json').write_text(json.dumps(summary, indent=2),
                                                   encoding='utf-8')
            if ((args.probe == 'v' and
                    (summary['published'] < 3 or summary['audio_stopped'] != 1)) or
                    summary['final_underruns'] != 0 or summary['decoder_faults'] != 0 or errors or
                    'app_render' not in metrics or 'app_send' not in metrics or
                    (changed is not None and (changed == 0 or outside != 0))):
                raise RuntimeError(f'output-source gate: {summary}')
            print('OUTPUT_SOURCE PASS', summary, flush=True)
        finally:
            port.close()


if __name__ == '__main__':
    main()
