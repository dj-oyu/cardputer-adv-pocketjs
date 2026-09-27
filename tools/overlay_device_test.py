"""Exercise both Kasane home overlays on a connected Cardputer.

The original HOME OVERLAY choice is restored in finally. Captures are the
RGB565 pixels handed to the LCD, not a host-side renderer approximation.
"""
import argparse
import re
import struct
import time
import zlib
from pathlib import Path

import serial


parser = argparse.ArgumentParser()
parser.add_argument('--port', required=True)
parser.add_argument('--out', type=Path, required=True)
parser.add_argument('--playback', action='store_true',
                    help='play the first already-granted audio file; never grant a new folder')
parser.add_argument('--fairness-seconds', type=float, default=0,
                    help='run the P5 fast-native-source deskclock diagnostic without captures')
parser.add_argument('--p1-nav-alias', action='store_true',
                    help='use diagnostic USB aliases for up/right; P0 reserves u/b')
parser.add_argument('--rearm-music-only', action='store_true',
                    help='toggle HOME OVERLAY off/on if its saved choice is music')
args = parser.parse_args()
args.out.mkdir(parents=True, exist_ok=True)
log = []


def png_chunk(kind, payload):
    return struct.pack('>I', len(payload)) + kind + payload + struct.pack('>I', zlib.crc32(kind + payload))


port = serial.Serial(port=None, baudrate=115200, timeout=0.15)
port.dtr = False
port.rts = False
port.port = args.port
with port:
    time.sleep(1.2)
    port.reset_input_buffer()

    def read_line():
        line = port.readline().decode(errors='replace').strip()
        if line:
            log.append(line)
        return line

    def await_line(marker, seconds=10, since=None, allow_stop=False):
        if since is not None:
            for line in log[since:]:
                if marker in line:
                    return line
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            line = read_line()
            if marker in line:
                return line
            if 'OVERLAY_REFUSED' in line or ('OVERLAY_STOPPED' in line and not allow_stop) or 'panic' in line.lower():
                raise RuntimeError(f'{marker}: {line}')
        raise RuntimeError(f'waiting for {marker}: {log[-12:]}')

    def await_any(markers, seconds=10, since=None):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            line = read_line()
            if since is not None and line not in log[since:]:
                continue
            if any(marker in line for marker in markers):
                return line
            if 'OVERLAY_REFUSED' in line or 'OVERLAY_STOPPED' in line or 'panic' in line.lower():
                raise RuntimeError(f'{markers}: {line}')
        raise RuntimeError(f'waiting for {markers}: {log[-12:]}')

    def key(value, marker):
        at = len(log)
        port.write(value.encode())
        return await_line(marker, since=at, allow_stop=marker == 'HOME_READY')

    def row():
        key('q', 'HOME_READY')
        # Returning from Settings and re-entering it in the same display tick
        # can consume the category key without opening the menu.
        time.sleep(0.15)
        key('>' if args.p1_nav_alias else 'b', 'CATEGORY 1')
        for _ in range(8):
            key('&' if args.p1_nav_alias else 'u', 'SELECT')
        for i in range(1, 5):
            key('d', f'SELECT {i}')

    def choice():
        row()
        line = key('e', 'OPEN 4 choice=')
        match = re.search(r'choice=(\d+)', line)
        if not match:
            raise RuntimeError(line)
        value = int(match.group(1))
        key('q', 'HOME_READY')
        return value

    def select(value):
        row()
        line = key('e', 'OPEN 4 choice=')
        match = re.search(r'choice=(\d+)', line)
        if not match:
            raise RuntimeError(line)
        current = int(match.group(1))
        step = 'd' if value > current else ('&' if args.p1_nav_alias else 'u')
        for n in range(current + (1 if value > current else -1),
                       value + (1 if value > current else -1),
                       1 if value > current else -1):
            key(step, f'CHOICE {n}')
        at = len(log)
        key('e', 'VALUE')
        if value:
            await_line('OVERLAY running state=', seconds=12, since=at)
            await_line('OVERLAY_COST ', since=at)
        return log[at:]

    def capture(name):
        key('s', 'CAPTURE_BEGIN')
        raw = bytearray()
        deadline = time.monotonic() + 18
        while time.monotonic() < deadline:
            raw.extend(port.read(65536))
            if b'CAPTURE_END' in raw:
                break
        rows = {}
        for y, data in re.findall(r'PIX (\d+) ([0-9a-f]{960})', raw.decode(errors='replace')):
            rgb = bytearray()
            for x in range(0, len(data), 4):
                v = int(data[x:x + 4], 16)
                rgb.extend((((v >> 11) & 31) * 255 // 31,
                            ((v >> 5) & 63) * 255 // 63,
                            (v & 31) * 255 // 31))
            rows[int(y)] = bytes(rgb)
        if len(rows) != 135 or any(len(r) != 720 for r in rows.values()):
            raise RuntimeError(f'{name}: incomplete LCD capture ({len(rows)} rows)')
        raster = b''.join(b'\0' + rows[y] for y in range(135))
        png = (b'\x89PNG\r\n\x1a\n' + png_chunk(b'IHDR', struct.pack('>IIBBBBB', 240, 135, 8, 2, 0, 0, 0))
               + png_chunk(b'IDAT', zlib.compress(raster)) + png_chunk(b'IEND', b''))
        (args.out / f'{name}.png').write_bytes(png)
        print(f'CAPTURE {name} rows={len(rows)}', flush=True)

    def try_playback():
        port.write(b'?')  # Close the help page before forwarding Enter to the app.
        time.sleep(0.5)
        at = len(log)
        port.write(b'e')
        line = await_any(('PICK ', 'PLAYER_FAIL'), seconds=12, since=at)
        if 'folders' in line:
            print('PLAYBACK_SKIP new folder grant required', flush=True)
            port.write(b'q')
            await_any(('GRANT DECLINED', 'PICK DECLINED'), since=len(log))
            return
        if 'PLAYER_FAIL' in line:
            print('PLAYBACK_SKIP ' + line, flush=True)
            return
        for _ in range(5):
            count = re.search(r'(\d+) rows', line)
            if count and int(count.group(1)) == 0:
                print('PLAYBACK_SKIP no audio file in the selected folder', flush=True)
                port.write(b'q')
                await_any(('PICK DECLINED',), since=len(log))
                return
            at = len(log)
            port.write(b'e')
            line = await_any(('PICK ENTER', 'PICKED ', 'PLAYER_FAIL'), seconds=12, since=at)
            if 'PICKED ' in line:
                break
            if 'PLAYER_FAIL' in line:
                print('PLAYBACK_SKIP ' + line, flush=True)
                return
        else:
            print('PLAYBACK_SKIP first path has no audio file within picker depth', flush=True)
            port.write(b'q')
            await_any(('PICK DECLINED',), since=len(log))
            return
        at = len(log)
        line = await_any(('PLAYER_OPEN ', 'PLAYER_FAIL '), seconds=20, since=at)
        if 'PLAYER_FAIL' in line:
            print('PLAYBACK_FAIL ' + line, flush=True)
            return
        print('PLAYBACK_OPEN ' + line, flush=True)
        time.sleep(1.2)
        capture('music_playing_early')
        time.sleep(3.0)
        capture('music_playing_late')
        print('PLAYBACK_CAPTURED', flush=True)

    original = None
    try:
        original = choice()
        print(f'ORIGINAL overlay={original}', flush=True)
        # A boot/start guard intentionally refuses re-arming an overlay until
        # the person turns it OFF. Make that transition explicit on every run.
        select(0)
        if args.rearm_music_only:
            if original != 2:
                raise RuntimeError(f'expected saved music choice 2, found {original}')
            select(2)
            print('MUSIC_REARMED', flush=True)
            raise SystemExit(0)
        if args.fairness_seconds:
            if args.fairness_seconds < 5:
                raise ValueError('fairness observation must be at least 5 seconds')
            started = select(1)
            await_line('KSN_FAIR BOUND', seconds=10,
                       since=len(log) - len(started))
            at = len(log)
            deadline = time.monotonic() + args.fairness_seconds
            while time.monotonic() < deadline:
                read_line()
            observed = log[at:]
            if any('OVERLAY_STOPPED' in line or 'OVERLAY_REFUSED' in line or
                   'KSN_FAIR ERROR' in line or 'panic' in line.lower()
                   for line in observed):
                raise RuntimeError(f'fairness overlay stopped: {observed[-15:]}')
            reports = [re.search(r'KSN_FAIR HEARTBEAT frames=(\d+) maxGapMs=([\d.]+)', line)
                       for line in observed]
            reports = [report for report in reports if report]
            if len(reports) < int(args.fairness_seconds) - 2:
                raise RuntimeError(f'only {len(reports)} guest heartbeats')
            frames = int(reports[-1].group(1))
            max_gap = float(reports[-1].group(2))
            print(f'FAIRNESS heartbeats={len(reports)} frames={frames} maxGapMs={max_gap}',
                  flush=True)
            before_stop = len(log)
            select(0)
            stopped = await_line('KSN_POOL: STOP published=', seconds=10,
                                 since=before_stop)
            print(stopped, flush=True)
            published = int(re.search(r'published=(\d+)', stopped).group(1))
            skipped = int(re.search(r'skipped=(\d+)', stopped).group(1))
            if (frames < int(args.fairness_seconds * 5) or max_gap > 200 or
                    published < int(args.fairness_seconds * 50) or skipped):
                raise RuntimeError('fast-native-source guest fairness gate failed')
            raise SystemExit(0)
        for value, name in ((1, 'deskclock'), (2, 'music')):
            started = select(value)
            for line in started:
                if 'OVERLAY_COST' in line or 'OVERLAY running' in line:
                    print(line, flush=True)
            capture(name)
            if value == 2:
                port.write(b'?')
                time.sleep(0.7)
                capture('music_help')
            deadline = time.monotonic() + 6
            at = len(log)
            while time.monotonic() < deadline:
                read_line()
            observed = log[at:]
            if any('OVERLAY_STOPPED' in line or 'OVERLAY_REFUSED' in line or 'panic' in line.lower()
                   for line in observed):
                raise RuntimeError(f'{name}: stopped during observation: {observed[-15:]}')
            perf = [line for line in observed if 'PERF mode=' in line]
            healthy = [line for line in observed if 'OVERLAY_HEALTHY' in line]
            print(f'RUNNING {name} perf_windows={len(perf)} healthy={len(healthy)}', flush=True)
            for line in perf[-2:] + healthy[-1:]:
                print(line, flush=True)
            if value == 2 and args.playback:
                try_playback()
            key('q', 'HOME_READY')
            print(await_line('OVERLAY_STOPPED', since=at), flush=True)
            print(f'BACK {name} OK', flush=True)
        select(0)
        at = len(log)
        deadline = time.monotonic() + 6
        while time.monotonic() < deadline:
            read_line()
        perf = [line for line in log[at:] if 'PERF mode=' in line]
        print(f'BASELINE perf_windows={len(perf)}', flush=True)
        for line in perf[-2:]:
            print(line, flush=True)
    finally:
        restore_error = None
        if original is not None:
            for attempt in range(2):
                try:
                    select(0)
                    if original:
                        select(original)
                    else:
                        key('q', 'HOME_READY')
                    print(f'RESTORED overlay={original}', flush=True)
                    restore_error = None
                    break
                except Exception as error:
                    restore_error = error
                    if attempt == 0:
                        time.sleep(0.25)
                    else:
                        print(f'RESTORE_FAILED {error}', flush=True)
        port.close()
        (args.out / 'serial.log').write_text('\n'.join(log) + '\n', encoding='utf-8')
        if restore_error is not None:
            raise RuntimeError('HOME OVERLAY restoration failed') from restore_error
