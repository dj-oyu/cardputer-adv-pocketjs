"""USB lifecycle smoke test. Run with the ESP-IDF Python environment."""
import argparse
import re
import time
import serial

p = argparse.ArgumentParser()
p.add_argument('--port', required=True)
p.add_argument('--cycles', type=int, default=100)
args = p.parse_args()
s = serial.Serial(args.port, 115200, timeout=0.2)

def command(value, marker):
    s.write(value.encode())
    deadline = time.monotonic() + 8
    lines = []
    while time.monotonic() < deadline:
        line = s.readline().decode(errors='replace').strip()
        if line:
            lines.append(line)
        if marker in line:
            return '\n'.join(lines)
    raise RuntimeError(f'Waiting for {marker}: {lines[-10:]}')

try:
    command('q', 'HOME_READY')
    command('a', 'CATEGORY 0')
    memory = []
    for i in range(args.cycles):
        command('e', 'HELLO_FRAME_PRESENTED')
        command('e', 'HELLO_COUNT 1')
        result = command('q', 'HOME_READY')
        match = re.search(r'MEM free=(\d+) largest=(\d+)', result)
        assert match, result
        memory.append(tuple(map(int, match.groups())))
        if (i+1) % 10 == 0:
            print('CYCLES', i+1, 'MEM', memory[-1], flush=True)
    assert memory[-1][0] >= memory[0][0] - 256, memory
    assert memory[-1][1] >= memory[0][1] - 256, memory
    for case in '123456':
        command(case, 'APP_STOPPED')
        command('q', 'HOME_READY')
        command('e', 'HELLO_FRAME_PRESENTED')
        command('q', 'HOME_READY')
        print('FAULT_RECOVERY_OK', case, flush=True)
    command('e', 'HELLO_FRAME_PRESENTED')
    print('SMOKE_OK', args.cycles, flush=True)
finally:
    s.close()
