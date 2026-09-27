"""Interactive Cardputer serial keys and LCD capture for device diagnosis.

Commands on stdin: key <letters>, wait <seconds>, shot <name>, quit.
The port opens with DTR/RTS disabled to avoid resetting playback.
"""
import argparse
from pathlib import Path
import re
import struct
import sys
import time
import zlib

import serial


parser = argparse.ArgumentParser()
parser.add_argument('--port', required=True)
parser.add_argument('--out', type=Path, required=True)
args = parser.parse_args()
args.out.mkdir(parents=True, exist_ok=True)


def chunk(kind, payload):
    return (struct.pack('>I', len(payload)) + kind + payload
            + struct.pack('>I', zlib.crc32(kind + payload)))


port = serial.Serial(port=None, baudrate=115200, timeout=0.1)
port.dtr = False
port.rts = False
port.port = args.port
port.open()
log = (args.out / 'serial.log').open('a', encoding='utf-8')
try:
    print('SERIAL_READY', flush=True)

    def read_for(seconds):
        until = time.monotonic() + seconds
        while time.monotonic() < until:
            raw = port.readline()
            if not raw:
                continue
            line = raw.decode(errors='replace').rstrip('\r\n')
            log.write(line + '\n')
            if not any(tag in line for tag in ('background: PERF', 'motion: ACC')):
                print(line, flush=True)
        log.flush()

    def shot(name):
        port.write(b's')
        raw = bytearray()
        until = time.monotonic() + 18
        while time.monotonic() < until and b'CAPTURE_END' not in raw:
            raw.extend(port.read(65536))
        rows = {}
        for y, data in re.findall(r'PIX (\d+) ([0-9a-f]{960})', raw.decode(errors='replace')):
            rgb = bytearray()
            for at in range(0, len(data), 4):
                value = int(data[at:at + 4], 16)
                rgb.extend((((value >> 11) & 31) * 255 // 31,
                            ((value >> 5) & 63) * 255 // 63,
                            (value & 31) * 255 // 31))
            rows[int(y)] = bytes(rgb)
        if len(rows) != 135:
            raise RuntimeError(f'incomplete capture: {len(rows)} rows')
        raster = b''.join(b'\0' + rows[y] for y in range(135))
        png = (b'\x89PNG\r\n\x1a\n'
               + chunk(b'IHDR', struct.pack('>IIBBBBB', 240, 135, 8, 2, 0, 0, 0))
               + chunk(b'IDAT', zlib.compress(raster)) + chunk(b'IEND', b''))
        path = args.out / f'{name}.png'
        path.write_bytes(png)
        print(f'SHOT {path}', flush=True)

    for command in sys.stdin:
        parts = command.strip().split(maxsplit=1)
        if not parts:
            continue
        if parts[0] == 'quit':
            break
        if parts[0] == 'key' and len(parts) == 2:
            port.write(parts[1].encode())
            read_for(2)
        elif parts[0] == 'wait' and len(parts) == 2:
            read_for(float(parts[1]))
        elif parts[0] == 'shot' and len(parts) == 2:
            shot(parts[1])
        else:
            print('COMMANDS key <letters> | wait <seconds> | shot <name> | quit', flush=True)
finally:
    log.close()
    port.close()
