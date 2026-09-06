"""Exercise the XMB categories, settings and mute option on the device."""
import argparse
import re
import time
import serial

p=argparse.ArgumentParser();p.add_argument('--port',required=True);a=p.parse_args()
s=serial.Serial(a.port,115200,timeout=0.1)
def command(key,marker):
    s.write(key.encode());end=time.monotonic()+5;lines=[]
    while time.monotonic()<end:
        line=s.readline().decode(errors='replace').strip();lines.append(line)
        if marker in line:return line
    raise RuntimeError(lines[-12:])
def value():
    line=command('e','VALUE')
    print(line,flush=True)
    m=re.search(r'background=(\d+) fps=(\d+) sound=(\d+)',line)
    assert m,line
    return tuple(map(int,m.groups()))
try:
    command('q','HOME_READY');command('a','CATEGORY 0');command('b','CATEGORY 1')
    command('u','SELECT');command('u','SELECT 0')
    first=value();second=value();assert first[0]!=second[0]
    command('d','SELECT 1');first=value();second=value();assert first[1]!=second[1]
    command('d','SELECT 2');state=value()
    if state[2]:state=value()
    assert state[2]==0
    time.sleep(0.3);s.read_all()
    command('u','SELECT 1');time.sleep(0.2)
    assert b'SFX' not in s.read_all(),'Sound emitted while muted'
    command('d','SELECT 2');assert value()[2]==1
    end=time.monotonic()+2;heard=False
    while time.monotonic()<end:
        if b'SFX 1 synthesized' in s.readline():heard=True;break
    assert heard,'Synthesized audio was not submitted to I2S'
    command('a','CATEGORY 0');command('e','HELLO_FRAME_PRESENTED');command('q','HOME_READY')
    print('SETTINGS_OK sound=ON categories, toggles, mute, app-return',flush=True)
finally:s.close()
