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
parser.add_argument('--regrant-music', action='store_true',
                    help='if this session has no grant, reselect the previously approved music folder')
parser.add_argument('--playback-observe-seconds', type=float, default=0,
                    help='observe real audio with FLOWER without LCD capture interference')
parser.add_argument('--fairness-seconds', type=float, default=0,
                    help='run the P5 fast-native-source deskclock diagnostic without captures')
parser.add_argument('--d6-video-seconds', type=float, default=0,
                    help='observe the diagnostic JS video overlay over FLOWER')
parser.add_argument('--d6-combined-faults', action='store_true',
                    help='add SYSTEM notice, 16 KiB reservation, and LCD retry')
parser.add_argument('--d6-audio-tone', action='store_true',
                    help='require accepted diagnostic tone windows during the D6 overlay')
parser.add_argument('--d6-sd-mp3', action='store_true',
                    help='pick the approved music folder and prove MP3 decoding with D6 video and FLOWER')
parser.add_argument('--d6-sd-av-stream', action='store_true',
                    help='run the audio-clock KSV1 + MP3 persistent SD coexistence overlay')
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
        captured = raw.decode(errors='replace')
        capture_events = [line.strip() for line in captured.splitlines()
                          if line.strip() and not line.startswith('PIX ')]
        log.extend(capture_events)
        if args.d6_video_seconds and any(
                'OVERLAY_STOPPED' in line or 'OVERLAY_REFUSED' in line or
                'D6_VIDEO_BUSY' in line or 'START_FAILED' in line or
                'panic' in line.lower() for line in capture_events):
            raise RuntimeError(f'{name}: capture diagnostics: {capture_events[-15:]}')
        rows = {}
        for y, data in re.findall(r'PIX (\d+) ([0-9a-f]{960})', captured):
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
            if not args.regrant_music:
                print('PLAYBACK_SKIP new folder grant required', flush=True)
                port.write(b'q')
                await_any(('GRANT DECLINED', 'PICK DECLINED'), since=len(log))
                return
            if 'PICK 2 folders' not in line:
                raise RuntimeError(f'unexpected SD grant screen: {line}')
            at = len(log)
            port.write(b'de')  # Existing card: second root row is music.
            granted = await_line('GRANTED ', since=at)
            if 'GRANTED music' not in granted:
                raise RuntimeError(f'not the previously approved folder: {granted}')
            line = await_line('PICK ', since=at)
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
        playback_at = len(log)
        if args.playback_observe_seconds:
            if args.playback_observe_seconds < 6:
                raise ValueError('playback observation must be at least 6 seconds')
            deadline = time.monotonic() + args.playback_observe_seconds
            while time.monotonic() < deadline:
                read_line()
            playing = log[playback_at:]
            perf = [line for line in playing if 'PERF mode=3' in line]
            if len(perf) < 2 or any('PLAYER_FAIL' in line or
                                   'OVERLAY_STOPPED' in line or
                                   'panic' in line.lower() for line in playing):
                raise RuntimeError(f'FLOWER/music playback did not continue: {playing[-15:]}')
            print(f'PLAYBACK_FLOWER_PERF windows={len(perf)}', flush=True)
            for line in perf:
                print(line, flush=True)
            return playback_at
        time.sleep(1.2)
        capture('music_playing_early')
        time.sleep(3.0)
        capture('music_playing_late')
        playing = log[playback_at:]
        if args.regrant_music:
            perf = [line for line in playing if 'PERF mode=3' in line]
            if len(perf) < 2 or any('PLAYER_FAIL' in line or
                                   'OVERLAY_STOPPED' in line or
                                   'panic' in line.lower() for line in playing):
                raise RuntimeError(f'FLOWER/music playback did not continue: {playing[-15:]}')
            print(f'PLAYBACK_FLOWER_PERF windows={len(perf)}', flush=True)
            for line in perf[-2:]:
                print(line, flush=True)
        print('PLAYBACK_CAPTURED', flush=True)
        return playback_at

    original = None
    lowheap_active = False
    notice_active = False
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
        if args.d6_sd_av_stream:
            select(1)
            at = len(log)
            port.write(b'e')
            await_line('D6_AV GRANT_REQUEST', seconds=8, since=at)
            grant = await_line('PICK ', seconds=12, since=at)
            if 'PICK 2 folders' not in grant:
                raise RuntimeError(f'unexpected SD grant: {grant}')
            port.write(b'de')
            granted = await_line('GRANTED ', seconds=12, since=at)
            if 'GRANTED music' not in granted:
                raise RuntimeError(f'not approved music: {granted}')
            staged = await_line('D6_AV STAGED bytes=157216', seconds=12, since=at)
            await_line('D6_AV STREAM_START', seconds=12, since=at)
            await_line('D6_AV MP3_OPEN codec=mp3', seconds=20, since=at)
            await_line('D6_AV MP3_PLAY', seconds=12, since=at)
            done = await_line('D6_AV DONE reason=end', seconds=25, since=at)
            if 'CLEANUP_ERROR' in done:
                raise RuntimeError(done)
            segment = log[at:]
            def index(marker):
                return next(i for i, line in enumerate(segment) if marker in line)
            def device_ms(line):
                match = re.search(r'\bI \((\d+)\)', line)
                if not match:
                    raise RuntimeError(f'missing device timestamp: {line}')
                return int(match.group(1))
            play_started_ms = device_ms(segment[index('D6_AV MP3_PLAY')])
            stream_ended_ms = device_ms(segment[index('D6_AV STREAM_STATE end')])
            playing = segment[index('D6_AV MP3_PLAY'):index('D6_AV EOF')]
            # PERF is a two-second aggregate. Accept only windows wholly
            # between playback start and stream EOF, not setup or tail time.
            steady_start_ms = play_started_ms + 2000
            play_perf = [line for line in playing if 'PERF mode=3' in line and
                         steady_start_ms <= device_ms(line) <= stream_ended_ms]
            play_heap = [line for line in playing if 'D6_AV: HEAP free=' in line and
                         steady_start_ms <= device_ms(line) <= stream_ended_ms]
            audio = [re.search(r'D6_AV MP3_STATUS second=(\d+) state=(\w+) '
                               r'positionMs=(\d+) underruns=(\d+)', line)
                     for line in playing]
            audio = [match for match in audio if match]
            selections = [(int(match.group(1)), int(match.group(2)))
                          for line in playing if (match := re.search(
                              r'VIDEO_STREAM: SELECT pts=(\d+) clock=(\d+)', line))]
            decoded = [re.search(r'MP3DEC packets=(\d+) frames=(\d+) faults=(\d+)', line)
                       for line in segment]
            decoded = [match for match in decoded if match]
            if len(play_perf) < 3 or len(play_heap) < 3:
                raise RuntimeError(f'phase windows play={len(play_perf)} '
                                   f'heap={len(play_heap)}')
            if len(selections) < 30 or any(a[0] >= b[0] for a, b in
                                            zip(selections, selections[1:])) or \
                    any(pts > clock for pts, clock in selections):
                raise RuntimeError(f'video/audio PTS progress: {selections[-8:]}')
            if len(audio) < 3 or int(audio[-1].group(3)) < 10000 or \
                    any(int(item.group(4)) for item in audio) or \
                    any(int(a.group(3)) > int(b.group(3)) for a, b in
                        zip(audio, audio[1:])):
                raise RuntimeError(f'MP3 position progress: {audio[-5:]}')
            if not decoded or int(decoded[-1].group(1)) <= 0 or \
                    int(decoded[-1].group(2)) <= 0 or int(decoded[-1].group(3)):
                raise RuntimeError(f'MP3 decoder: {decoded}')
            if not any('D6_AV STOP ack=true' in line for line in segment) or \
                    not any('D6_AV FILE_RETAINED' in line for line in segment) or \
                    not any('D6_AV MP3_STOP ' in line for line in segment) or \
                    any('D6_AV ERROR ' in line or 'OVERLAY_STOPPED' in line or
                        'panic' in line.lower() for line in segment):
                raise RuntimeError(f'cleanup/playback failed: {segment[-20:]}')
            fps = [float(re.search(r'fps=([\d.]+)', line).group(1))
                   for line in play_perf]
            free = [int(re.search(r'HEAP free=(\d+)', line).group(1))
                    for line in play_heap]
            lag = [clock - pts for pts, clock in selections]
            if min(fps) < 27.0 or max(lag) > 100000 or \
                    len(selections) < 280 or selections[-1][0] < 9900000:
                raise RuntimeError(f'D6 SD AV performance gate: '
                                   f'fps_min={min(fps):.1f} '
                                   f'lag_max_us={max(lag)} '
                                   f'selected={len(selections)} '
                                   f'pts_last={selections[-1][0]}')
            print(f'D6_SD_AV_PASS play_perf={len(play_perf)} heap={len(play_heap)} '
                  f'selected={len(selections)} pts_last={selections[-1][0]} '
                  f'audio_ms={audio[-1].group(3)} '
                  f'fps_steady_min={min(fps):.1f} heap_free_min={min(free)} '
                  f'pts_lag_max_us={max(lag)} '
                  f'packets={decoded[-1].group(1)} faults=0', flush=True)
            print(staged, flush=True)
            print(done, flush=True)
            raise SystemExit(0)
        if args.d6_video_seconds:
            if args.d6_video_seconds < 6:
                raise ValueError('D6 video observation must be at least 6 seconds')
            started = select(1)
            await_line('D6_VIDEO_FRAME tick=0', seconds=12,
                       since=len(log) - len(started))
            if args.d6_sd_mp3:
                if args.d6_combined_faults or args.d6_audio_tone:
                    raise ValueError('D6 SD MP3 run uses its own playback observation')
                at = len(log)
                port.write(b'e')
                await_line('D6_SD_MP3_REQUEST', seconds=6, since=at)
                grant = await_line('PICK ', seconds=12, since=at)
                if 'PICK 2 folders' not in grant:
                    raise RuntimeError(f'unexpected SD grant screen: {grant}')
                at = len(log)
                port.write(b'de')
                granted = await_line('GRANTED ', since=at)
                if 'GRANTED music' not in granted:
                    raise RuntimeError(f'not the previously approved folder: {granted}')
                await_line('D6_SD_MP3_GRANTED', since=at)
                line = await_line('PICK ', since=at)
                for _ in range(5):
                    count = re.search(r'(\d+) rows', line)
                    if count and int(count.group(1)) == 0:
                        raise RuntimeError('no MP3 in the approved folder')
                    at = len(log)
                    port.write(b'e')
                    line = await_any(('PICK ENTER', 'PICKED '), seconds=12, since=at)
                    if 'PICKED ' in line:
                        break
                else:
                    raise RuntimeError('first folder path contains no MP3 within picker depth')
                picked = await_line('D6_SD_MP3_PICKED ', seconds=12, since=at)
                if '01 KAKATORO.mp3' not in picked:
                    raise RuntimeError(f'unexpected first SD MP3: {picked}')
                opened = await_line('D6_SD_MP3_OPEN codec=mp3', seconds=20, since=at)
                played = await_line('D6_SD_MP3_PLAY', seconds=12, since=at)
                print(picked, flush=True)
                print(opened, flush=True)
                print(played, flush=True)
            at = len(log)
            deadline = time.monotonic() + args.d6_video_seconds
            while time.monotonic() < deadline:
                read_line()
            observed = log[at:]
            if any('OVERLAY_STOPPED' in line or 'OVERLAY_REFUSED' in line or
                   'D6_VIDEO_BUSY' in line or 'D6_SD_MP3_ERROR' in line or
                   'panic' in line.lower()
                   for line in observed):
                raise RuntimeError(f'D6 video overlay stopped or stalled: {observed[-15:]}')
            frames = [int(match.group(1)) for line in observed
                      if (match := re.search(r'D6_VIDEO_FRAME tick=(\d+)', line))]
            perf = [line for line in observed if 'PERF mode=3' in line]
            if len(frames) < int(args.d6_video_seconds) - 2 or len(perf) < 2:
                raise RuntimeError(f'D6 video progress frames={frames} perf={len(perf)}')
            if args.d6_audio_tone:
                on = [line for line in observed
                      if re.search(r'FLOWER_AUDIO_PROBE: tone=1 available=1', line)]
                requests = [int(match.group(1)) for line in log
                            if (match := re.search(
                                r'FLOWER_AUDIO_PROBE: tone_request=(-?\d+)', line))]
                if not on or not requests or any(value <= 0 for value in requests):
                    raise RuntimeError(f'D6 tone unavailable: on={len(on)} requests={requests}')
                print(f'D6_AUDIO_PASS windows={len(on)} requests={len(requests)}', flush=True)
            print(f'D6_VIDEO_PASS reports={len(frames)} last_tick={frames[-1]} '
                  f'flower_perf={len(perf)}', flush=True)
            for line in perf[-2:]:
                print(line, flush=True)
            if args.d6_sd_mp3:
                if not any('D6_SD_MP3_PLAY' in line for line in log):
                    raise RuntimeError('MP3 playback did not start')
                select(0)
                summary = await_line('MP3DEC packets=', seconds=12, since=at,
                                     allow_stop=True)
                match = re.search(r'MP3DEC packets=(\d+) frames=(\d+) faults=(\d+)',
                                  summary)
                if not match or int(match.group(1)) <= 0 or \
                   int(match.group(2)) <= 0 or int(match.group(3)) != 0:
                    raise RuntimeError(f'MP3 decoder failed: {summary}')
                print(f'D6_SD_MP3_PASS packets={match.group(1)} '
                      f'frames={match.group(2)} faults={match.group(3)} '
                      f'video_last_tick={frames[-1]} flower_perf={len(perf)}',
                      flush=True)
                raise SystemExit(0)
            capture('d6_video_flower')
            if args.d6_combined_faults:
                combined_at = len(log)
                key('H', 'KSN_P5_LOWHEAP: ON bytes=16384')
                lowheap_active = True
                key('U', 'KSN_P5_NOTICE: POST result=0')
                notice_active = True
                await_line('KSN_P5_NOTICE: COMPOSITED 1', seconds=8)
                at = len(log)
                tone_line = None
                if args.d6_audio_tone:
                    tone_line = await_line('FLOWER_AUDIO_PROBE: tone_request=',
                                           seconds=6, since=at)
                    tone_id = re.search(r'tone_request=(-?\d+)', tone_line)
                    if not tone_id or int(tone_id.group(1)) <= 0:
                        raise RuntimeError(f'D6 tone request rejected: {tone_line}')
                    at = len(log)
                port.write(b'%')
                await_line('KSN_P5_REPAIR: ARMED after=3', seconds=8, since=at)
                fail_line = await_line('KSN_P5_REPAIR: INJECT_FAIL', seconds=8, since=at)
                repair_line = await_line('KSN_P5_REPAIR: REPAIR_OK', seconds=8, since=at)
                if tone_line:
                    timestamps = [re.search(r'\((\d+)\)', line)
                                  for line in (tone_line, fail_line, repair_line)]
                    if any(match is None for match in timestamps):
                        raise RuntimeError('D6 tone/repair device timestamp missing')
                    tone_ms, fail_ms, repair_ms = (int(match.group(1)) for match in timestamps)
                    if not (0 <= fail_ms - tone_ms < 1900 and
                            0 <= repair_ms - tone_ms < 1900 and
                            fail_ms <= repair_ms):
                        raise RuntimeError(f'D6 repair missed tone window: '
                                           f'{tone_ms}, {fail_ms}, {repair_ms}')
                    print(f'D6_TONE_REPAIR_OVERLAP fail_ms={fail_ms-tone_ms} '
                          f'repair_ms={repair_ms-tone_ms}', flush=True)
                progress_at = len(log)
                video_line = await_line('D6_VIDEO_FRAME tick=', seconds=3,
                                        since=progress_at)
                video_tick = int(re.search(r'tick=(\d+)', video_line).group(1))
                if video_tick <= frames[-1]:
                    raise RuntimeError(f'D6 video did not advance: {video_tick}')
                flower_line = await_line('PERF mode=3', seconds=4,
                                         since=progress_at)
                capture('d6_video_flower_notice')
                combined = log[combined_at:]
                if any('OVERLAY_STOPPED' in line or 'OVERLAY_REFUSED' in line or
                       'D6_VIDEO_BUSY' in line or 'START_FAILED' in line or
                       'panic' in line.lower() for line in combined):
                    raise RuntimeError(f'D6 combined diagnostic failure: {combined[-15:]}')
                key('I', 'KSN_P5_NOTICE: CLEAR')
                notice_active = False
                await_line('KSN_P5_NOTICE: COMPOSITED 0', seconds=8)
                key('H', 'KSN_P5_LOWHEAP: OFF')
                lowheap_active = False
                print(f'D6_COMBINED_PASS notice=1 lowheap=16384 retry=1 '
                      f'video_tick={video_tick} flower={flower_line}', flush=True)
            select(0)
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
            playback_at = try_playback() if value == 2 and args.playback else None
            key('q', 'HOME_READY')
            if playback_at is not None and args.playback_observe_seconds:
                decoded = [re.search(r'MP3DEC packets=(\d+) frames=(\d+) faults=(\d+)', line)
                           for line in log[playback_at:]]
                decoded = [match for match in decoded if match]
                if not decoded or not any(int(match.group(1)) > 0 and
                                          int(match.group(2)) > 0 and
                                          int(match.group(3)) == 0
                                          for match in decoded):
                    raise RuntimeError('no successful MP3 decode summary after playback')
                print(f'PLAYBACK_MP3 packets={decoded[-1].group(1)} '
                      f'frames={decoded[-1].group(2)} faults={decoded[-1].group(3)}',
                      flush=True)
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
        cleanup_error = None
        if notice_active:
            try:
                key('I', 'KSN_P5_NOTICE: CLEAR')
            except Exception as error:
                cleanup_error = error
        if lowheap_active:
            try:
                key('H', 'KSN_P5_LOWHEAP: OFF')
            except Exception as error:
                if cleanup_error is None:
                    cleanup_error = error
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
        if cleanup_error is not None:
            raise RuntimeError('D6 diagnostic cleanup failed') from cleanup_error
