"""Confirm that the normal firmware reaches HOME_READY after a device probe."""
import argparse
import time

import serial


parser = argparse.ArgumentParser()
parser.add_argument('--port', default='COM3')
args = parser.parse_args()

port = serial.Serial(port=None, baudrate=115200, timeout=0.1)
port.dtr = False
port.rts = False
port.port = args.port
data = bytearray()
with port:
    deadline = time.monotonic() + 12
    while time.monotonic() < deadline and b'HOME_READY' not in data:
        data.extend(port.read(32768))
    if b'HOME_READY' not in data:
        port.write(b'q')
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline and b'HOME_READY' not in data:
            data.extend(port.read(32768))
print('HOME_READY', b'HOME_READY' in data)
raise SystemExit(0 if b'HOME_READY' in data else 2)
