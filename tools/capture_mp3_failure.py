"""Capture serial diagnostics while a person reproduces an MP3 playback fault.

The script does not send input or reset the board. It closes COM on exit.
"""
import argparse
from pathlib import Path
import time

import serial


parser = argparse.ArgumentParser()
parser.add_argument('--port', required=True)
parser.add_argument('--out', type=Path, required=True)
parser.add_argument('--seconds', type=int, default=180)
args = parser.parse_args()
args.out.parent.mkdir(parents=True, exist_ok=True)

interesting = ('PLAYER_', 'MP3DEC', 'pocket.av', 'sd_media', 'sd:',
               'sound:', 'Guru Meditation', 'E (', 'W (')
deadline = time.monotonic() + args.seconds
failure_at = None

port = serial.Serial(port=None, baudrate=115200, timeout=0.2)
# Avoid the monitor-open reset that would interrupt an already playing track.
port.dtr = False
port.rts = False
port.port = args.port
port.open()
try:
    with args.out.open('w', encoding='utf-8') as log:
        print('CAPTURE_READY', flush=True)
        while time.monotonic() < deadline:
            raw = port.readline()
            if not raw:
                continue
            line = raw.decode(errors='replace').rstrip('\r\n')
            log.write(line + '\n')
            log.flush()
            if any(marker in line for marker in interesting):
                print(line, flush=True)
            if 'PLAYER_FAIL PLAYBACK' in line and failure_at is None:
                failure_at = time.monotonic()
            if failure_at is not None and time.monotonic() - failure_at >= 3:
                break
finally:
    port.close()

print(f'CAPTURE_DONE {args.out}', flush=True)
