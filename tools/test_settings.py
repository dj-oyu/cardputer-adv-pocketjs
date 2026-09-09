"""Exercise settings. MP3 PLAYBACK is appended to Apps; category 1 and all
settings row indices used below are unchanged. The overlay row is now a
three-way choice (OFF / DESK CLOCK / MUSIC) rather than a toggle."""
import argparse
import re
import time
import serial
from home_modes import BACKGROUNDS

p=argparse.ArgumentParser();p.add_argument('--port',required=True);a=p.parse_args()
s=serial.Serial(a.port,115200,timeout=0.1)
time.sleep(1.5);s.reset_input_buffer()
def command(key,marker):
    s.write(key.encode());end=time.monotonic()+5;lines=[]
    while time.monotonic()<end:
        line=s.readline().decode(errors='replace').strip();lines.append(line)
        if marker in line:return line
    raise RuntimeError(lines[-12:])
def expect(marker,timeout=8):
    end=time.monotonic()+timeout;lines=[]
    while time.monotonic()<end:
        line=s.readline().decode(errors='replace').strip();lines.append(line)
        if marker in line:return line
    raise RuntimeError(lines[-12:])
def value(selected):
    command('e','OPEN')
    for _ in range(len(BACKGROUNDS)-1):command('u','CHOICE')
    for _ in range(selected):command('d','CHOICE')
    line=command('e','VALUE')
    print(line,flush=True)
    m=re.search(r'background=(\d+) fps=(\d+) sound=(\d+)',line)
    assert m,line
    return tuple(map(int,m.groups()))
try:
    command('q','HOME_READY');command('a','CATEGORY 0');command('b','CATEGORY 1')
    command('u','SELECT');command('u','SELECT');command('u','SELECT 0')
    first=value(0);second=value(1);assert first[0]==0 and second[0]==1
    for mode in range(2,len(BACKGROUNDS)):assert value(mode)[0]==mode
    value(1)
    # Back cancels the pending choice without applying it.
    command('e','OPEN');command('u','CHOICE 0');command('q','HOME_READY')
    line=command('e','OPEN');assert 'choice=1' in line,line
    command('q','HOME_READY')
    command('d','SELECT 1');first=value(1);second=value(0);assert first[1]==1 and second[1]==0
    command('d','SELECT 2');state=value(0)
    assert state[2]==0
    time.sleep(0.3);s.read_all()
    command('u','SELECT 1');time.sleep(0.2)
    assert b'SFX' not in s.read_all(),'Sound emitted while muted'
    command('d','SELECT 2');assert value(1)[2]==1
    end=time.monotonic()+2;heard=False
    while time.monotonic()<end:
        # Logged after the last I2S write of the click, not when it was queued.
        if b'SFX 1 played' in s.readline():heard=True;break
    assert heard,'Synthesized audio was not submitted to I2S'
    # WI-FI is the first SETTING_ACTION row: Enter leaves the home screen for
    # the picker rather than opening a value list, and the picker starts a scan
    # on arrival. Escape comes back mid-scan; the scan finishes on its own and
    # puts the radio down, which is what SCAN_DONE reports. No credentials are
    # entered here, and nothing associates.
    command('d','SELECT 3')
    line=command('e','SCREEN 3');assert 'screen=1' in line,line
    # SCAN_START first: wifi_ui starts the scan before it announces the screen,
    # so waiting for WIFI_READY and then for SCAN_START waits for a line that
    # has already gone past.
    expect('SCAN_START');expect('WIFI_READY')
    s.write(b'\x1b');expect('HOME_READY')
    # Waited for, not slept through: the app launched below shares the heap the
    # radio is still holding until the scan task exits.
    expect('SCAN_DONE',20)
    # HOME OVERLAY is the row of docs/common-api.md 3.1, appended after WI-FI.
    # Opened and left without applying: choosing one ENDS the menu and starts a
    # JS guest in its place, and this script's job is the menu.
    #
    # The value it opens on is NOT asserted. It used to be `choice=0`, which
    # read as a contract and was really an assumption that nobody had ever
    # turned the overlay on -- the moment somebody did, this failed on a device
    # that was working correctly. What is being checked here is that the row is
    # at index 4 and that opening and leaving it changes nothing.
    command('d','SELECT 4')
    line=command('e','OPEN 4');assert 'choice=' in line,line
    command('q','HOME_READY')
    command('b','CATEGORY 1');command('u','SELECT 3');command('u','SELECT 2')
    # POCKET PET is appended at index 5; existing settings/Hello indices stay fixed.
    command('a','CATEGORY 0');command('e','HELLO_FRAME_PRESENTED');command('q','HOME_READY')
    print('SETTINGS_OK sound=ON categories, toggles, mute, app-return',flush=True)
finally:
    # An assertion above can abort while SOUND is OFF, which then persists in NVS.
    # Walk back to Settings > SOUND > ON so the device is never left muted.
    try:
        # home, Settings, top, SOUND, open, choose ON, apply. Four ups, because
        # an abort can leave the cursor on DESK CLOCK, the last row, and three
        # would stop short of the top — and the fourth row's Enter opens a
        # screen, not a value list.
        for key in 'qbuuuuddede':s.write(key.encode());time.sleep(0.4)
    except Exception:pass
    s.close()
