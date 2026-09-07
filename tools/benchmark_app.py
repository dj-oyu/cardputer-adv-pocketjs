"""Measure the JS app frame on the device: start the hello app from the home
screen, keep it repainting with Enter presses, and print the PAINT lines
(turn_ms / render_ms / kernel_ms / send_ms, 30-frame averages).

    python tools/benchmark_app.py --port COM3 [--samples 6]

Run with the ESP-IDF Python environment (pyserial).
"""
import argparse
import re
import time
import serial

p = argparse.ArgumentParser()
p.add_argument('--port', required=True)
p.add_argument('--samples', type=int, default=6, help='PAINT lines to collect')
p.add_argument('--interval', type=float, default=0.25, help='seconds between Enter presses')
p.add_argument('--sound', choices=['on', 'off'], help='set the sound option first (each Enter plays a sound otherwise)')
a = p.parse_args()
s = serial.Serial(a.port, 115200, timeout=0.05)
time.sleep(1.5)
s.reset_input_buffer()


def wait(marker, limit=8):
    end = time.monotonic() + limit
    seen = []
    while time.monotonic() < end:
        line = s.readline().decode(errors='replace').strip()
        if line:
            seen.append(line)
        if marker in line:
            return line
    raise RuntimeError(f'waiting for {marker}: {seen[-8:]}')


try:
    s.write(b'q'); wait('HOME_READY')
    if a.sound:
        # settings category, third item, open, pick OFF/ON, confirm
        s.write(b'b'); wait('CATEGORY 1')
        s.write(b'd'); wait('SELECT'); s.write(b'd'); wait('SELECT 2')
        s.write(b'e'); wait('OPEN')
        s.write(b'd' if a.sound == 'on' else b'u'); wait('CHOICE')
        s.write(b'e'); wait('VALUE')
        s.write(b'u'); wait('SELECT'); s.write(b'u'); wait('SELECT 0')
    s.write(b'a'); wait('CATEGORY 0')
    s.write(b'e'); wait('HELLO_FRAME_PRESENTED')
    got, fields = [], []
    last = time.monotonic()
    while len(got) < a.samples:
        if time.monotonic() - last >= a.interval:
            s.write(b'e'); last = time.monotonic()
        line = s.readline().decode(errors='replace').strip()
        if 'PAINT' in line:
            print(line, flush=True)
            got.append(line)
            fields.append({k: float(v) for k, v in re.findall(r'(\w+_ms)=([\d.]+)', line)})
    if fields:
        keys = fields[0].keys()
        print('MEAN ' + ' '.join(f'{k}={sum(f[k] for f in fields) / len(fields):.2f}' for k in keys), flush=True)
    s.write(b'q'); wait('HOME_READY')
finally:
    s.close()
