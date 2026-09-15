"""Kasane-only admission, services, display and repeated teardown memory check."""
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
            if 'Guru Meditation' in line or 'START_FAILED' in line:
                raise RuntimeError(line)
            if marker in line:
                return '\n'.join(lines)
        raise RuntimeError('missing ' + marker + ': ' + repr(lines[-6:]))

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
        command(b'e', 'APP_REFUSED KASANE_ONLY')
        command(b'q', 'HOME_READY')
        for i in range(a.cycles):
            boot = command(b'K', 'KASANE_FRAME_PRESENTED')
            assert 'KASANE_SERVICES PASS legacy=false' in boot, boot
            command(b'b', 'KASANE_ACTION right release held=false')
            end = command(b'q', 'HOME_READY')
            match = re.search(r'MEM free=(\d+) largest=(\d+) js=0', end)
            assert match, end
            memory.append(tuple(map(int, match.groups())))
            if (i + 1) % 10 == 0:
                print('KASANE_CYCLES', i + 1, 'free/largest', memory[-1], flush=True)
        assert min(m[0] for m in memory) >= memory[0][0] - 256, memory
        assert min(m[1] for m in memory) >= memory[0][1] - 256, memory
        print('KASANE_ONLY_DEVICE PASS', a.cycles, memory[-1], flush=True)
    finally:
        (a.out / 'serial.log').write_text('\n'.join(log) + '\n', encoding='utf-8')
        (a.out / 'memory.json').write_text(json.dumps(memory), encoding='utf-8')
