"""Probe-only, volatile notification/timer pressure test. Does not write NVS."""
import argparse
from pathlib import Path
import re
import struct
import time
import zlib
import serial

p = argparse.ArgumentParser()
p.add_argument('--port', required=True)
p.add_argument('--out', type=Path, required=True)
a = p.parse_args()
a.out.mkdir(parents=True, exist_ok=True)
log = []
with serial.Serial(a.port, 115200, timeout=0.1) as port:
    def wait(marker, timeout=12):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            line = port.readline().decode(errors='replace').strip()
            if line:
                log.append(line)
            if any(x in line for x in ('Guru Meditation', 'SYS_PROBE_BUSY', 'SYS_PROBE_ERROR')):
                raise RuntimeError(line)
            if marker in line:
                return line
        raise RuntimeError('missing ' + marker + ': ' + repr(log[-8:]))

    def state(key='O'):
        port.write(key.encode())
        line = wait('SYS_PROBE active=')
        return {k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', line)}

    def press(key):
        port.write(key.encode())
        time.sleep(0.35)

    def capture():
        port.write(b's')
        wait('CAPTURE_BEGIN')
        data = bytearray()
        end = time.monotonic() + 18
        while time.monotonic() < end:
            data.extend(port.read(65536))
            if b'CAPTURE_END' in data:
                break
        rows = {}
        for y, text in re.findall(rb'PIX (\d+) ([0-9a-f]{960})', data):
            rgb = bytearray()
            for x in range(0, 960, 4):
                v = int(text[x:x+4], 16)
                rgb.extend((((v >> 11) & 31)*255//31, ((v >> 5) & 63)*255//63, (v & 31)*255//31))
            rows[int(y)] = bytes(rgb)
        assert set(rows) == set(range(135)), len(rows)
        def chunk(kind, payload):
            return struct.pack('>I', len(payload)) + kind + payload + struct.pack('>I', zlib.crc32(kind + payload))
        png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 240, 135, 8, 2, 0, 0, 0))
        png += chunk(b'IDAT', zlib.compress(b''.join(b'\0' + rows[y] for y in range(135)))) + chunk(b'IEND', b'')
        (a.out / 'notice.png').write_bytes(png)

    try:
        time.sleep(1)
        state('Z')
        press('q')
        state('N')
        time.sleep(0.5)
        full = state()
        assert (full['active'], full['queued'], full['blocked']) == (1, 8, 1), full
        capture()
        press('b')
        refused = state()
        assert refused['id'] == full['id'] and refused['snoozed'] == 0, refused
        press('e')
        retry = state()
        assert retry['id'] != full['id'] and retry['queued'] == 8 and retry['timers'] == 0, retry
        press('e')
        press('b')
        snoozed = state()
        assert (snoozed['active'], snoozed['queued'], snoozed['snoozed']) == (1, 6, 1), snoozed
        cleared = state('Z')
        assert all(cleared[k] == 0 for k in ('active', 'queued', 'snoozed', 'timers', 'blocked')), cleared
        print('SYSTEM_DEVICE_OK 8+1, full snooze refusal, timer retry, ACK, snooze, owner cleanup', flush=True)
    finally:
        port.write(b'Z')
        time.sleep(0.2)
        (a.out / 'serial.log').write_text('\n'.join(log), encoding='utf-8')
