"""Exercise the shipped MP3 player, including alternate-cycle pause/resume."""
import argparse
import re
import time
import serial
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('--port', default='COM3')
p.add_argument('--cycles', type=int, default=20)
a = p.parse_args()
s = serial.Serial(a.port, 115200, timeout=0.2)

def duration(name):
    data=(Path(__file__).resolve().parents[1]/'apps/mp3play'/name).read_bytes()
    at=0; samples=0; rate=0
    while at<len(data):
        h=data[at:at+4]; assert len(h)==4 and h[0]==255
        v=(h[1]>>3)&3; b=h[2]>>4
        rate=[44100,48000,32000][(h[2]>>2)&3]>>(0 if v==3 else 1 if v==2 else 2)
        rates=[0,32,40,48,56,64,80,96,112,128,160,192,224,256,320] if v==3 else [0,8,16,24,32,40,48,56,64,80,96,112,128,144,160]
        at+=(144000 if v==3 else 72000)*rates[b]//rate+((h[2]>>1)&1)
        samples+=1152 if v==3 else 576
    assert at==len(data)
    return (samples*24000//rate)*1000//24000

sources=['test-tone.mp3','test-48k.mp3','test-24k.mp3']
durations=[duration(name) for name in sources]

def wait(marker, seconds=10):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        line = s.readline().decode(errors='replace').strip()
        if line:
            print(line, flush=True)
        if 'MP3_FAIL' in line or 'Guru Meditation' in line:
            raise RuntimeError(line)
        if marker in line:
            return line
    raise RuntimeError('Timeout waiting for ' + marker)

try:
    s.write(b'q'); wait('HOME_READY')
    s.write(b'a'); wait('CATEGORY 0')
    # Down clamps at the final row. No dependency on the previous selection.
    for _ in range(11):
        s.write(b'd'); time.sleep(0.1)
    s.write(b'e')
    for cycle in range(a.cycles):
        line = wait('MP3_DONE cycle=' + str(cycle), 30)
        assert '"state":"ended"' in line, line
        assert '"underruns":0' in line, line
        assert 'src='+sources[cycle%3] in line, line
        assert 'duration='+str(durations[cycle%3]) in line, line
    s.write(b'q'); wait('HOME_READY')
    print('MP3_DEVICE_OK cycles=' + str(a.cycles), flush=True)
finally:
    s.close()
