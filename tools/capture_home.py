"""Capture actual LCD pixels and measure navigation with audio enabled."""
import argparse
from pathlib import Path
import struct
import re
import time
import zlib
import serial

p = argparse.ArgumentParser()
p.add_argument('--port', required=True)
a = p.parse_args()
out = Path(__file__).resolve().parents[1] / '.cache/xmb'
out.mkdir(parents=True, exist_ok=True)

def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))

with serial.Serial(a.port, 115200, timeout=0.2) as s:
    time.sleep(1.5)
    s.reset_input_buffer()

    def command(keys, marker):
        s.write(keys.encode())
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            line = s.readline().decode(errors='replace')
            if marker in line:
                return line
        raise RuntimeError(marker)

    def capture(name):
        time.sleep(0.4)
        command('s', 'CAPTURE_BEGIN')
        rows = {}
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            line = s.readline().decode(errors='replace').strip()
            match = re.search(r'PIX (\d+) ([0-9a-f]{960})', line)
            if match:
                y, data = match.groups()
                rgb = bytearray()
                for x in range(0, len(data), 4):
                    v = int(data[x:x+4], 16)
                    rgb.extend((((v >> 11) & 31)*255//31, ((v >> 5) & 63)*255//63, (v & 31)*255//31))
                rows[int(y)] = bytes(rgb)
            if 'CAPTURE_END' in line:
                break
        assert len(rows) == 135 and all(len(r) == 720 for r in rows.values()), len(rows)
        raw = b''.join(b'\0' + rows[y] for y in range(135))
        png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 240, 135, 8, 2, 0, 0, 0))
        png += chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')
        (out / (name + '.png')).write_bytes(png)
        print('CAPTURE', name, flush=True)

    command('q', 'HOME_READY')
    command('a', 'CATEGORY 0')
    capture('apps')  # POCKET PET is appended; original navigation indices are unchanged.
    command('b', 'CATEGORY 1')
    command('uu', 'SELECT 0')
    command('d', 'SELECT 1')
    capture('settings')
    command('e', 'OPEN')
    capture('choices')
    command('q', 'HOME_READY')
    # Allow capture overhead to leave the performance window.
    time.sleep(2.5)
    s.reset_input_buffer()
    records = []
    for mode in range(3):
        command('uu', 'SELECT 0')
        command('e', 'OPEN')
        command('u', 'CHOICE');command('u', 'CHOICE')
        for _ in range(mode):command('d', 'CHOICE')
        command('e', 'VALUE')
        deadline = time.monotonic() + 8
        sequence = 'dduueudqab'
        step = 0
        next_key = 0
        while time.monotonic() < deadline:
            if time.monotonic() >= next_key:
                s.write(sequence[step % len(sequence)].encode())
                step += 1
                next_key = time.monotonic() + 0.08
            line = s.readline().decode(errors='replace').strip()
            if 'PERF' in line:
                records.append(line)
                print('MOVING', line, flush=True)
        command('q', 'HOME_READY')
        command('b', 'CATEGORY 1')
    (out / 'navigation-performance.txt').write_text('\n'.join(records), encoding='utf-8')
    command('a', 'CATEGORY 0')
