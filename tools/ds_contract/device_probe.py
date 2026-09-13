"""Trigger target diagnostics, retain the log and check pre-SPI pixels."""
import argparse
from pathlib import Path
import re
import struct
import time
import zlib
import serial


def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    data = bytearray()
    with serial.Serial(args.port, 115200, timeout=0.2) as port:
        time.sleep(1.5)
        port.reset_input_buffer()
        port.write(b'q')
        deadline = time.monotonic() + 8
        ready = bytearray()
        while time.monotonic() < deadline:
            ready.extend(port.read(4096))
            if b'HOME_READY' in ready:
                break
        else:
            raise RuntimeError('Could not return to HOME_READY')
        port.reset_input_buffer()
        port.write(b'~')
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline:
            data.extend(port.read(65536))
            if b'HOME_READY' in data:
                break
    (args.out / 'serial.log').write_bytes(data)
    log = data.decode(errors='replace')
    for line in log.splitlines():
        if any(marker in line for marker in ('DS_PROBE', 'PASS', 'failure', 'panic', 'HOME_READY')):
            print(line)
    if ('DS_PROBE: PASS' not in log or 'HOME_READY' not in log or 'DS_PROBE: FAIL' in log
            or not re.search(r'DS_PROBE: PARTIAL us=\d+ mask=fe0 bytes=26880', log)
            or 'DS_PROBE: UNCHANGED bands=0 bytes=0' not in log):
        raise RuntimeError('Diagnostic did not pass and return to the home loop; see serial.log')
    rows = {int(y): bytes.fromhex(pixels) for y, pixels in re.findall(r'PIX (\d+) ([0-9a-f]{960})', log)}
    if set(rows) != set(range(135)):
        raise RuntimeError(f'Incomplete capture: {len(rows)} rows')
    raw = bytearray()
    for y in range(135):
        raw.append(0)
        for x in range(240):
            value = struct.unpack_from('>H', rows[y], x * 2)[0]
            rgb = (0x0b, 0x17, 0x27)
            if 160 <= x < 224 and 40 <= y < 96:
                rgb = (0x67, 0xdf, 0xc7)
            if 8 <= x < 232 and 8 <= y < 24:
                rgb = (0xf5, 0xbb, 0x69)
            r, g, b = rgb
            expected = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            if value != expected:
                raise RuntimeError(f'Pixel mismatch at {x},{y}: {value:04x} != {expected:04x}')
            raw.extend((((value >> 11) & 31) * 255 // 31,
                        ((value >> 5) & 63) * 255 // 63, (value & 31) * 255 // 31))
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 240, 135, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')
    (args.out / 'pre-spi.png').write_bytes(png)
    print('PRE_SPI_PIXELS PASS 32400 pixels; physical LCD appearance is not read back')


if __name__ == '__main__':
    main()
