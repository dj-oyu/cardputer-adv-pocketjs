"""The scenario: edit in the Playground, leave WITHOUT saving, care for the pet,
come back and find the edit. Driven over USB; srcstore's own log lines are the
evidence (ESC over USB is the screen's Back, so it is how "leave unsaved" is
performed here, and the editor logs no byte count on the way out).

The Playground holds the person's own program, so this puts it back: the typed
characters are deleted again and the record is saved only once its length
matches what was there on arrival.

    python draft_scenario.py --port COM3
"""
import argparse, re, time
import serial

TYPED = "// draft line"

p = argparse.ArgumentParser()
p.add_argument('--port', required=True)
a = p.parse_args()
s = serial.Serial(a.port, 115200, timeout=0.2)
time.sleep(1.5); s.reset_input_buffer()


def send(keys, marker, limit=15):
    s.write(keys.encode())
    end = time.monotonic() + limit
    lines = []
    while time.monotonic() < end:
        line = s.readline().decode(errors='replace').strip()
        if line:
            lines.append(line)
            if marker in line:
                return lines
    raise RuntimeError(f'waiting for {marker}: ' + ' / '.join(lines[-6:]))


def find(lines, pattern):
    for line in lines:
        m = re.search(pattern, line)
        if m:
            return m
    return None


def to_row(row, marker):
    send('a', 'CATEGORY 0')
    for _ in range(8):
        s.write(b'u'); time.sleep(0.12)
    s.reset_input_buffer()
    for n in range(1, row + 1):
        send('d', f'APP {n}')
    return send('e', marker, 20)


print('--- open the Playground')
send('q', 'HOME_READY')
lines = to_row(2, 'CODE_READY')              # 0 HELLO, 1 SKK, 2 PLAYGROUND
m = find(lines, r'slot 0: loaded (\d+) bytes')
base = int(m.group(1)) if m else 0
print('   record on arrival:', base, 'bytes')
assert base > 0

print('--- type, then leave with Back, saving nothing')
s.write(b'i'); time.sleep(0.4)
s.write(TYPED.encode()); time.sleep(0.8)
# The first ESC leaves insert mode (the vim engine takes it); the second is the
# screen's Back, which is how a person walks away with the edit unsaved.
send('\x1b', 'VIM')
lines = send('\x1b', 'HOME_READY')
m = find(lines, r'draft: parked (\d+) bytes of slot (\d+)')
assert m, 'nothing was parked: ' + ' / '.join(lines[-6:])
print('   parked', m.group(1), 'bytes for slot', m.group(2))
assert int(m.group(1)) == base + len(TYPED), (base, m.group(1))

print('--- care for the pet')
to_row(5, 'FRAME_PRESENTED')                 # 5 = POCKET PET
time.sleep(6)
send('q', 'HOME_READY')

print('--- back to the Playground')
lines = to_row(2, 'CODE_READY')
m = find(lines, r'draft: (\d+) bytes for slot (\d+)')
assert m, 'the draft was not taken back: ' + ' / '.join(lines[-6:])
print('   restored', m.group(1), 'bytes from the draft')
assert int(m.group(1)) == base + len(TYPED)

print('--- put the Playground back as it was')
for _ in range(len(TYPED)):
    s.write(b'x'); time.sleep(0.15)
lines = send('\x13', 'slot 0: saved')        # C-s
m = find(lines, r'slot 0: saved (\d+) bytes')
print('   saved', m.group(1), 'bytes back')
assert int(m.group(1)) == base, (base, m.group(1))
lines = send('\x1b', 'HOME_READY')
assert not find(lines, r'draft: parked'), 'a draft was parked after the save'
print('OK: the unsaved edit survived the pet; the record is back to', base, 'bytes')
s.close()
