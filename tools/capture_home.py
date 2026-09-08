"""Capture actual LCD pixels and measure navigation with audio enabled."""
import argparse
from pathlib import Path
import struct
import re
import time
import zlib
import serial
from home_modes import BACKGROUNDS

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

    # A capture is 135 lines of 960 hex characters -- about 131 KB arriving over
    # roughly a second, on top of whatever else is logging.
    #
    # Read it in bulk and parse afterwards. Decoding each line as it arrives is
    # what loses rows: the work per line is enough that the host stops draining
    # the port, the operating system's own buffer overflows, and the missing
    # rows come back scattered with the line before each gap truncated
    # mid-hex. That looks exactly like the firmware dropping log output, and it
    # was diagnosed as such here for most of a day -- the same capture that gave
    # 81, 85 and 92 rows line-by-line gives 135 first time when the port is
    # drained into a buffer and parsed at the end.
    def attempt():
        command('s', 'CAPTURE_BEGIN')
        buf = bytearray()
        deadline = time.monotonic() + 18
        while time.monotonic() < deadline:
            buf += s.read(65536)
            if b'CAPTURE_END' in buf:
                break
        rows = {}
        for y, data in re.findall(r'PIX (\d+) ([0-9a-f]{960})', buf.decode(errors='replace')):
            rgb = bytearray()
            for x in range(0, len(data), 4):
                v = int(data[x:x+4], 16)
                rgb.extend((((v >> 11) & 31)*255//31, ((v >> 5) & 63)*255//63, (v & 31)*255//31))
            rows[int(y)] = bytes(rgb)
        return rows

    # One retry remains for the genuinely unlucky run. The assert stays exact:
    # a capture with a hole in it is not a screenshot and must never be written
    # out as one just because most of the rows arrived.
    def capture(name):
        time.sleep(0.4)
        rows = attempt()
        if len(rows) != 135:
            print('CAPTURE retry', name, len(rows), flush=True)
            time.sleep(0.6)
            rows = attempt()
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
    for mode in range(len(BACKGROUNDS)):
        command('uu', 'SELECT 0')
        command('e', 'OPEN')
        for _ in range(len(BACKGROUNDS)-1):command('u', 'CHOICE')
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
