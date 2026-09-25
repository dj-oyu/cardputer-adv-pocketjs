"""Smoke and capture the mounted foreground apps without changing pet storage.

Pet is tested last and left running; reset the board after this script. Sending
Back to pet would save (and potentially normalize) the user's persisted pet.
"""
import argparse
from pathlib import Path
import re
import struct
import time
import zlib

import serial


parser = argparse.ArgumentParser()
parser.add_argument('--port', required=True)
parser.add_argument('--out', type=Path, required=True)
args = parser.parse_args()
args.out.mkdir(parents=True, exist_ok=True)
log = []


def chunk(kind, payload):
    return struct.pack('>I', len(payload)) + kind + payload + struct.pack('>I', zlib.crc32(kind + payload))


try:
    with serial.Serial(args.port, 115200, timeout=0.15) as port:
        time.sleep(1.2)
        port.reset_input_buffer()

        def read_line():
            line = port.readline().decode(errors='replace').strip()
            if line:
                log.append(line)
                if any(bad in line for bad in ('Guru Meditation', 'PRESENTER_STEP_FAILED', 'START_FAILED')):
                    raise RuntimeError(line)
            return line

        def wait(marker, seconds=12, since=0):
            for line in log[since:]:
                if marker in line:
                    return line
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                line = read_line()
                if marker in line:
                    return line
            raise RuntimeError(f'{marker}: {log[-12:]}')

        def key(value, marker):
            at = len(log)
            port.write(value.encode())
            return wait(marker, since=at)

        def open_app(index, ready):
            key('q', 'HOME_READY')
            key('a', 'CATEGORY 0')
            for _ in range(8):
                key('u', 'APP ')
            for number in range(1, index + 1):
                key('d', f'APP {number}')
            at = len(log)
            key('e', 'KASANE_FRAME_PRESENTED')
            wait(ready, since=at)
            print(f'OPEN {index} {ready}', flush=True)

        def capture(name):
            key('s', 'CAPTURE_BEGIN')
            raw = bytearray()
            deadline = time.monotonic() + 18
            while time.monotonic() < deadline:
                raw.extend(port.read(65536))
                if b'CAPTURE_END' in raw:
                    break
            rows = {}
            for y, data in re.findall(r'PIX (\d+) ([0-9a-f]{960})', raw.decode(errors='replace')):
                rgb = bytearray()
                for x in range(0, len(data), 4):
                    value = int(data[x:x + 4], 16)
                    rgb.extend((((value >> 11) & 31) * 255 // 31,
                                ((value >> 5) & 63) * 255 // 63,
                                (value & 31) * 255 // 31))
                rows[int(y)] = bytes(rgb)
            if len(rows) != 135 or any(len(row) != 720 for row in rows.values()):
                raise RuntimeError(f'{name}: incomplete LCD capture ({len(rows)} rows)')
            raster = b''.join(b'\0' + rows[y] for y in range(135))
            png = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 240, 135, 8, 2, 0, 0, 0))
                   + chunk(b'IDAT', zlib.compress(raster)) + chunk(b'IEND', b''))
            (args.out / f'{name}.png').write_bytes(png)
            print(f'CAPTURE {name} rows=135', flush=True)

        open_app(0, 'HELLO_READY')
        key('e', 'HELLO_COUNT 1')
        capture('hello')
        open_app(4, 'IMUCAL_READY')
        capture('imucal')
        open_app(6, 'COMPANION_READY')
        capture('companion')
        open_app(5, 'PET_READY')
        capture('pet')
        print('PET_LEFT_RUNNING reset without Back to preserve storage', flush=True)
finally:
    (args.out / 'serial.log').write_text('\n'.join(log) + '\n', encoding='utf-8')
