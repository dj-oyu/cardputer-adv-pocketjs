"""Exercise migrated hello input, display, idle and repeated memory recovery."""
import argparse
import json
from pathlib import Path
import re
import time
import serial

p = argparse.ArgumentParser()
p.add_argument('--port', required=True)
p.add_argument('--out', type=Path, required=True)
p.add_argument('--cycles', type=int, default=100)
a = p.parse_args()
assert a.cycles > 0
a.out.mkdir(parents=True, exist_ok=True)
log, memory = [], []
with serial.Serial(a.port, 115200, timeout=0.15) as port:
    def wait(marker):
        lines = []
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            line = port.readline().decode(errors='replace').strip()
            if line:
                lines.append(line)
                log.append(line)
            if any(word in line for word in ('Guru Meditation', 'START_FAILED', 'listener failed')):
                raise RuntimeError(line)
            if marker in line:
                return '\n'.join(lines)
        raise RuntimeError('missing ' + marker + ': ' + repr(lines[-8:]))

    def command(key, marker):
        time.sleep(0.15)
        port.write(key)
        return wait(marker)

    try:
        time.sleep(1)
        port.reset_input_buffer()
        command(b'q', 'HOME_READY')
        command(b'a', 'CATEGORY 0')
        for _ in range(8):
            command(b'u', 'APP ')
        for i in range(a.cycles):
            boot = command(b'e', 'KASANE_FRAME_PRESENTED')
            assert 'HELLO_READY' in boot and 'APP_ID local.hello' in boot, boot
            command(b'e', 'HELLO_COUNT 1')
            command(b'e', 'HELLO_COUNT 2')
            if i == 0:
                # read large chunks: byte-at-a-time readline can overflow the
                # USB receive queue during a 130 KB pre-SPI capture.
                time.sleep(0.15)
                port.write(b's')
                capture = bytearray()
                deadline = time.monotonic() + 10
                while b'CAPTURE_END' not in capture and time.monotonic() < deadline:
                    capture.extend(port.read(65536))
                text = capture.decode(errors='replace')
                log.extend(text.splitlines())
                rows = re.findall(r'PIX (\d+) [0-9a-f]{960}', text)
                assert set(map(int, rows)) == set(range(135)), 'incomplete capture'
            end = command(b'q', 'HOME_READY')
            match = re.search(r'MEM free=(\d+) largest=(\d+) js=0', end)
            assert match, end
            memory.append(tuple(map(int, match.groups())))
            if (i + 1) % 10 == 0:
                print('HELLO_CYCLES', i + 1, 'free/largest', memory[-1], flush=True)
        assert min(m[0] for m in memory) >= memory[0][0] - 256, memory
        assert min(m[1] for m in memory) >= memory[0][1] - 256, memory
        print('HELLO_DEVICE PASS', a.cycles, memory[-1], flush=True)
    finally:
        (a.out / 'serial.log').write_text('\n'.join(log) + '\n', encoding='utf-8')
        (a.out / 'memory.json').write_text(json.dumps(memory), encoding='utf-8')
