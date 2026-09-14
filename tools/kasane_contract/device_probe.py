"""Trigger target diagnostics, retain the log and check pre-SPI pixels."""
import argparse
from pathlib import Path
import re
import struct
import time
import zlib
import serial
import json
from frost_reference import snapshot, gallery_pixel
from stress_reference import source as stress_source, pixel as stress_pixel


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
        deadline = time.monotonic() + 240
        pending = bytearray()
        while time.monotonic() < deadline:
            incoming = port.read(65536)
            data.extend(incoming)
            pending.extend(incoming)
            while b'\n' in pending:
                line, _, rest = pending.partition(b'\n')
                pending = bytearray(rest)
                text = line.decode(errors='replace')
                if any(marker in text for marker in ('KSN_PROBE', 'PASS', 'failure', 'panic', 'HOME_READY')):
                    print(text, flush=True)
            if b'HOME_READY' in data:
                break
    (args.out / 'serial.log').write_bytes(data)
    log = data.decode(errors='replace')
    if ('KSN_PROBE: PASS' not in log or 'HOME_READY' not in log or 'KSN_PROBE: FAIL' in log
            or not re.search(r'KSN_PROBE: PARTIAL us=\d+ mask=fe0 bytes=26880', log)
            or 'KSN_PROBE: UNCHANGED bands=0 bytes=0' not in log
            or 'KSN_PROBE: CACHE templates=1 instances=2 commands=2 native=4104' not in log
            or 'KSN_PROBE: COMPOSITION group_alpha=128 modal=open-close focus=42 PASS' not in log
            or 'KSN_PROBE: GLASS PASS' not in log
            or 'KSN_PROBE: VIEW PASS' not in log
            or 'KSN_PROBE: PIE_AB PASS' not in log
            or 'KSN_PROBE: STRESS PASS frames=600' not in log):
        raise RuntimeError('Diagnostic did not pass and return to the home loop; see serial.log')
    rows = {int(y): bytes.fromhex(pixels) for y, pixels in re.findall(r'PIX (\d+) ([0-9a-f]{960})', log.split('GLASS_PIX_BEGIN')[0])}
    if set(rows) != set(range(135)):
        raise RuntimeError(f'Incomplete capture: {len(rows)} rows')
    raw = bytearray()
    for y in range(135):
        raw.append(0)
        for x in range(240):
            value = struct.unpack_from('>H', rows[y], x * 2)[0]
            rgb = (0x0b, 0x17, 0x27)
            if 160 <= x < 224 and 40 <= y < 96:
                premultiplied = [0x67, 0xdf, 0xc7]
                if 176 <= x < 208 and 56 <= y < 80:
                    premultiplied = [(c * 128 + 127) // 255 + (d * 127 + 127) // 255
                                     for c, d in zip((0xf5, 0xbb, 0x69), premultiplied)]
                # Group opacity applies once after the two children are composed.
                rgb = tuple(min(255, (p * 128 + 127) // 255 + (bg * 127 + 127) // 255)
                            for p, bg in zip(premultiplied, (8, 20, 33)))
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
    glass_log = log.split('GLASS_PIX_BEGIN', 1)[1].split('GLASS_PIX_END', 1)[0]
    rows = {int(y): bytes.fromhex(pixels) for y, pixels in re.findall(r'PIX (\d+) ([0-9a-f]{960})', glass_log)}
    if set(rows) != set(range(135)):
        raise RuntimeError(f'Incomplete glass capture: {len(rows)} rows')
    blurred = snapshot(2)
    raw = bytearray()
    for y in range(135):
        raw.append(0)
        for x in range(240):
            value = struct.unpack_from('>H', rows[y], x*2)[0]
            expected = gallery_pixel(blurred, x, y)
            if value != expected:
                raise RuntimeError(f'Glass pixel mismatch at {x},{y}: {value:04x} != {expected:04x}')
            raw.extend((((value >> 11) & 31)*255//31, ((value >> 5) & 63)*255//63, (value & 31)*255//31))
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 240, 135, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')
    (args.out / 'glass-pre-spi.png').write_bytes(png)
    print('GLASS_PIXELS PASS 32400 pixels (alpha left / frost right)')
    captures = re.findall(r'STRESS_PIX_BEGIN frame=(\d+) tick=(\d+) radius=(\d+)(.*?)STRESS_PIX_END', log, re.S)
    if [int(c[0]) for c in captures] != [0, 299, 599]:
        raise RuntimeError('Missing temporal stress captures')
    ticks = [int(c[1]) for c in captures]
    if not ticks[0] < ticks[1] < ticks[2]:
        raise RuntimeError('Animation time did not advance')
    for frame, tick, radius, section in captures:
        t = int(tick)
        image = snapshot(int(radius), lambda x, y: stress_source(x, y, t))
        rows = {int(y): bytes.fromhex(p) for y, p in re.findall(r'PIX (\d+) ([0-9a-f]{960})', section)}
        if set(rows) != set(range(135)):
            raise RuntimeError(f'Incomplete stress capture {frame}')
        raw = bytearray()
        for y in range(135):
            raw.append(0)
            for x in range(240):
                value = struct.unpack_from('>H', rows[y], x*2)[0]
                expected = stress_pixel(image, x, y, t)
                if value != expected:
                    raise RuntimeError(f'Stress frame {frame} mismatch at {x},{y}: {value:04x} != {expected:04x}')
                raw.extend((((value >> 11) & 31)*255//31, ((value >> 5) & 63)*255//63, (value & 31)*255//31))
        png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 240, 135, 8, 2, 0, 0, 0))
        png += chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')
        (args.out / f'stress-{int(frame):03d}.png').write_bytes(png)
    report = {}
    for category in ('timing', 'split', 'cadence', 'memory'):
        match = re.search(r'STRESS '+category+r' ([^\r\n]+)', log)
        if not match:
            raise RuntimeError(f'Missing stress {category}')
        report[category] = {key: int(value) for key, value in re.findall(r'(\w+)=(\d+)', match[1])}
    if report['cadence']['bytes'] != 600*64800:
        raise RuntimeError('Incomplete stress display workload')
    report['observed_fps'] = 600*1e6/report['cadence']['elapsed_us']
    report['capture_frames'] = [int(c[0]) for c in captures]
    (args.out / 'stress-report.json').write_text(json.dumps(report, indent=2)+'\n')
    print('STRESS_PIXELS PASS 97200 pixels across frames 0/299/599')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
