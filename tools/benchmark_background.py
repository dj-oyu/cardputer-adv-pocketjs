"""Measure both backgrounds after flashing/resetting the device."""
import argparse
from pathlib import Path
import time
import serial

p=argparse.ArgumentParser()
p.add_argument('--port',required=True)
a=p.parse_args()
out=Path(__file__).resolve().parents[1]/'.cache/backgrounds'
out.mkdir(parents=True,exist_ok=True)
s=serial.Serial(a.port,115200,timeout=0.2)
records=[]

def read_until(marker, timeout=15):
    lines=[]
    end=time.monotonic()+timeout
    while time.monotonic()<end:
        line=s.readline().decode(errors='replace').strip()
        lines.append(line)
        if marker in line:return lines
    raise RuntimeError((marker,lines[-8:]))

try:
    s.write(b'q');read_until('HOME_READY')
    s.write(b'b');read_until('CATEGORY 1')
    s.write(b'uu');read_until('SELECT 0')
    # Test the saved background first, then the other background.
    for mode in range(2):
        if mode:
            s.write(b'e');read_until('VALUE')
        for sample in range(4):
            lines=read_until('PERF')
            for line in lines:
                if 'PERF' in line:
                    print(line,flush=True);records.append(line)
    (out/'performance.txt').write_text('\n'.join(records),encoding='utf-8')
finally:s.close()
