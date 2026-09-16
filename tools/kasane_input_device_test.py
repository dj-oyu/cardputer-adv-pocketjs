"""CP5 input integration smoke. K diagnostic; leaves the device at home.

Also launches hello and pet (without feeding or changing pet selection).
Requires the normal firmware with apps/kasane/demo.js embedded.
"""
import argparse
from pathlib import Path
import time
import serial

parser = argparse.ArgumentParser()
parser.add_argument('--port', required=True)
parser.add_argument('--out', type=Path, required=True)
args = parser.parse_args()
args.out.mkdir(parents=True, exist_ok=True)
log = []

with serial.Serial(args.port, 115200, timeout=0.15) as port:
    def expect(marker, seconds=10):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            line = port.readline().decode(errors='replace').strip()
            if line:
                log.append(line)
                if any(s in line for s in ('READY', 'COUNT', 'KASANE_ACTION', 'KASANE_TEXT')):
                    print(line, flush=True)
                if any(s in line for s in ('Guru Meditation', 'START_FAILED', 'listener failed')):
                    raise RuntimeError(line)
                if marker in line:
                    return
        raise RuntimeError('missing ' + marker + ': ' + repr(log[-8:]))

    def command(keys, marker):
        time.sleep(0.15)  # Log markers can precede the owner turn's screen switch.
        port.write(keys)
        expect(marker)

    try:
        time.sleep(1)
        port.reset_input_buffer()
        port.write(b'\x1b')  # Cancel a field left by an interrupted previous run.
        time.sleep(0.2)
        command(b'\x1b', 'HOME_READY')
        command(b'a', 'CATEGORY 0')
        for _ in range(8):
            command(b'u', 'APP ')
        command(b'e', 'HELLO_READY')
        command(b'e', 'HELLO_COUNT 1')
        command(b'e', 'HELLO_COUNT 2')
        command(b'q', 'HOME_READY')
        for i in range(1, 6):
            command(b'd', 'APP ' + str(i))
        command(b'e', 'PET_READY')
        # Navigate away and back without activating a mutating pet action.
        port.write(b'ba')
        time.sleep(0.5)
        command(b'q', 'HOME_READY')

        for _ in range(2):
            command(b'K', 'KASANE_READY active=true')
            command(b'b', 'KASANE_ACTION right press held=true')
            expect('KASANE_ACTION right release held=false')
            command(b'e', 'KASANE_ACTION accept press held=true')
            expect('KASANE_ACTION accept release held=false')
            # The USB keyboard switches to text routing at the end of this
            # owner turn, after the synchronous release callback has logged.
            time.sleep(0.15)
            command(b'h', 'KASANE_TEXT_EDIT h')
            command(b'i', 'KASANE_TEXT_EDIT hi')
            command(b'\r', 'KASANE_TEXT_SUBMIT hi')
            command(b'e', 'KASANE_ACTION accept press held=true')
            expect('KASANE_ACTION accept release held=false')
            time.sleep(0.15)
            command(b'\x1b', 'KASANE_TEXT_CANCEL')
            command(b'q', 'HOME_READY')
        print('KASANE_INPUT_DEVICE PASS', flush=True)
    finally:
        (args.out / 'serial.log').write_text('\n'.join(log) + '\n', encoding='utf-8')
