"""One reproducible Cardputer music trial; never flashes or writes the SD card.

Start with a freshly booted music overlay, configured with HOME OVERLAY=2 and
the test card inserted. This script owns the serial port only for this trial.
"""
import argparse
import json
from pathlib import Path
import re
import time

import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--play-seconds', type=float, default=90)
    parser.add_argument('--pause-after', type=float, default=30)
    parser.add_argument('--pause-seconds', type=float, default=2)
    parser.add_argument('--removal-window-seconds', type=float, default=0,
                        help='wait after track 02 for a person to remove/reinsert SD; skips pause')
    parser.add_argument('--removal-paused', action='store_true',
                        help='pause track 02 before the manual SD removal window')
    parser.add_argument('--track-cycles', type=int, default=0,
                        help='advance through this folder repeatedly as a lifecycle stress test')
    parser.add_argument('--advance-tracks', type=int, default=0,
                        help='advance this many tracks after track 02 before timed playback')
    parser.add_argument('--lowheap', action='store_true',
                        help='reserve 16 KiB internal heap before opening the picker; needs P5 probe')
    parser.add_argument('--text-pie', choices=('off', 'on'),
                        help='set binary-text PIE before launch; needs KASANE_TEXT_PIE_DEVICE_PROBE')
    parser.add_argument('--output-source-fast', action='store_true',
                        help='enable the 33 ms audio output source before playback; needs P1 overlay probe')
    parser.add_argument('--output-source-bind-visible', action='store_true',
                        help='bind that source to the visible music status line; needs P1 overlay probe')
    parser.add_argument('--output-source-after-open', action='store_true',
                        help='enable the 33 ms source after track 01 opens but before track 02; needs P1 overlay probe')
    parser.add_argument('--reset-p0-at-track-2', action='store_true',
                        help='reset diagnostic timing counters after track 02 opens')
    parser.add_argument('--inject-repair-after', type=float, default=0,
                        help='inject one overlay LCD failure this many seconds into track 02')
    parser.add_argument('--notice-after', type=float, default=0,
                        help='post SYSTEM notice during track 02, clear 2 seconds later')
    parser.add_argument('--switch-hello', action='store_true',
                        help='after leaving music overlay, launch/exit Hello World')
    parser.add_argument('--product-smoke', action='store_true',
                        help='normal playback smoke on a build without P0 diagnostic markers')
    args = parser.parse_args()
    if not args.product_smoke and not args.removal_window_seconds and not args.track_cycles and (
            args.play_seconds < args.pause_after + 10 or args.pause_after < 5):
        parser.error('play-seconds must exceed pause-after by at least 10 seconds')
    if args.product_smoke and (args.lowheap or args.text_pie or args.output_source_fast or
            args.output_source_after_open or args.output_source_bind_visible or
            args.reset_p0_at_track_2 or args.inject_repair_after or args.notice_after or
            args.removal_window_seconds or args.removal_paused or args.track_cycles or
            args.advance_tracks):
        parser.error('--product-smoke is separate from diagnostic probes and navigation stress')
    if args.removal_window_seconds and args.removal_window_seconds < 20:
        parser.error('removal-window-seconds must be at least 20')
    if args.removal_paused and not args.removal_window_seconds:
        parser.error('removal-paused requires removal-window-seconds')
    if args.track_cycles < 0 or (args.track_cycles and args.removal_window_seconds):
        parser.error('track-cycles must be nonnegative and separate from removal test')
    if args.advance_tracks < 0 or (args.advance_tracks and
                                  (args.track_cycles or args.removal_window_seconds)):
        parser.error('advance-tracks must be nonnegative and used only in timed playback')
    if args.output_source_fast and args.output_source_after_open:
        parser.error('choose one output-source activation point')
    if args.output_source_bind_visible and not (
            args.output_source_fast or args.output_source_after_open):
        parser.error('--output-source-bind-visible needs an output source')
    if args.inject_repair_after and (args.inject_repair_after < 1 or
            args.inject_repair_after >= args.pause_after or args.track_cycles or
            args.removal_window_seconds):
        parser.error('repair injection must be within timed playback, before pause')
    if args.notice_after and (args.notice_after < 1 or
            args.notice_after + 3 >= args.pause_after or args.inject_repair_after or
            args.track_cycles or args.removal_window_seconds):
        parser.error('SYSTEM notice must fit before pause and be separate from repair')
    if args.out.exists() and any(args.out.iterdir()):
        parser.error('--out must be a new or empty directory')
    args.out.mkdir(parents=True, exist_ok=True)

    lines = []
    marks = []
    with (args.out / 'serial.log').open('w', encoding='utf-8') as log:
        port = serial.Serial(port=None, baudrate=115200, timeout=0.1)
        port.dtr = False
        port.rts = False
        port.port = args.port
        port.open()
        try:
            def collect(seconds, pattern=None):
                deadline = time.monotonic() + seconds
                while time.monotonic() < deadline:
                    raw = port.readline()
                    if not raw:
                        continue
                    line = raw.decode(errors='replace').rstrip('\r\n')
                    lines.append(line)
                    log.write(line + '\n')
                    if pattern and re.search(pattern, line):
                        log.flush()
                        print('SEEN', line, flush=True)
                        return line
                log.flush()
                if pattern:
                    raise TimeoutError(f'no {pattern!r} within {seconds}s')
                return None

            def key(value):
                port.write(value.encode('ascii'))
                marks.append({'t': time.monotonic(), 'key': value})
                print('KEY', value, flush=True)

            if args.text_pie:
                key('K' if args.text_pie == 'on' else 'k')
                collect(4, rf'KSN_PIE: TEXT {int(args.text_pie == "on")}')
            if args.lowheap:
                key('H')
                collect(4, r'KSN_P5_LOWHEAP: ON bytes=16384 free=\d+ largest=\d+')
            if args.output_source_fast:
                key('&')
                collect(8, r'KSN_P1_OUTPUT_OVERLAY ACTIVE sampleMs=33')
                if args.output_source_bind_visible:
                    key('d')
                    collect(8, r'KSN_P1_OUTPUT_OVERLAY BOUND visible=1')

            # These are the exact picker transitions for the approved SD folder.
            key('e')
            collect(12, r'pocket\.sd: PICK 2 folders')
            key('de')
            collect(12, r'pocket\.sd: GRANTED music')
            collect(12, r'pocket\.pick: PICK 1 rows')
            key('e')
            collect(12, r'pocket\.pick: PICK ENTER KAKATO rows=1')
            key('e')
            collect(12, r'pocket\.pick: PICK ENTER KAKATO/KARA OK 2nd Edition rows=20')
            key('e')
            collect(12, r'pocket\.pick: PICKED .*01 KAKATORO\.mp3')
            collect(12, r'PLAYER_OPEN .*01 KAKATORO\.mp3')
            if args.output_source_after_open:
                key('&')
                collect(8, r'KSN_P1_OUTPUT_OVERLAY ACTIVE sampleMs=33')
                if args.output_source_bind_visible:
                    key('d')
                    collect(8, r'KSN_P1_OUTPUT_OVERLAY BOUND visible=1')
            collect(35, r'PLAYER_OPEN .*02 インザハウス\.mp3')
            if args.reset_p0_at_track_2:
                key('R')
                collect(4, r'KSN_P0: RESET requested')
            # In the P0 + P1 overlay build, USB 'b' launches a foreground
            # diagnostic app instead of navigating right inside music.
            next_track_key = '>' if (args.output_source_fast or
                                     args.output_source_after_open) else 'b'
            for _ in range(args.advance_tracks):
                collect(0.5)
                key(next_track_key)
                collect(8, r'PLAYER_OPEN ')

            if args.product_smoke:
                collect(args.play_seconds)
            elif args.track_cycles:
                for _ in range(args.track_cycles):
                    collect(0.5)
                    key(next_track_key)
                    collect(8, r'PLAYER_OPEN ')
                collect(2)
            elif args.removal_window_seconds:
                if args.removal_paused:
                    collect(5)
                    key('e')
                    collect(8, r'transition=mp3_paused')
                print('READY_REMOVE_SD', flush=True)
                collect(args.removal_window_seconds)
            else:
                if args.inject_repair_after:
                    collect(args.inject_repair_after)
                    key('%')
                    collect(8, r'KSN_P5_REPAIR: INJECT_FAIL y=24 after=3')
                    collect(8, r'KSN_P5_REPAIR: REPAIR_OK bands=17 bytes=64800')
                    collect(args.pause_after - args.inject_repair_after)
                elif args.notice_after:
                    collect(args.notice_after)
                    key('J')
                    collect(4, r'KSN_P5_NOTICE: POST result=0 id=\d+')
                    collect(2)
                    key('C')
                    collect(4, r'KSN_P5_NOTICE: CLEAR')
                    collect(args.pause_after - args.notice_after - 2)
                else:
                    collect(args.pause_after)
                key('e')
                collect(8, r'transition=mp3_paused')
                collect(args.pause_seconds)
                key('e')
                collect(8, r'transition=mp3_playing')
                collect(args.play_seconds - args.pause_after)
            key('q')
            collect(12, r'app: APP_STOPPED')
            if args.switch_hello:
                collect(0.25)
                key('a')
                collect(8, r'shell: CATEGORY 0')
                key('e')
                collect(12, r'KASANE_FRAME_PRESENTED')
                key('e')
                collect(8, r'HELLO_COUNT 1\b')
                key('q')
                collect(12, r'app: APP_STOPPED')
        finally:
            port.close()

    summary = {
        'port': args.port,
        'play_seconds': args.play_seconds,
        'pause_after': args.pause_after,
        'pause_seconds': args.pause_seconds,
        'removal_window_seconds': args.removal_window_seconds,
        'removal_paused': args.removal_paused,
        'track_cycles': args.track_cycles,
        'advance_tracks': args.advance_tracks,
        'lowheap': args.lowheap,
        'text_pie': args.text_pie,
        'output_source_fast': args.output_source_fast,
        'output_source_after_open': args.output_source_after_open,
        'reset_p0_at_track_2': args.reset_p0_at_track_2,
        'inject_repair_after': args.inject_repair_after,
        'notice_after': args.notice_after,
        'switch_hello': args.switch_hello,
        'product_smoke': args.product_smoke,
        'marks': marks,
        'events': [line for line in lines if ('KSN_P0:' in line or 'MP3DEC' in line
                                            or 'PLAYER_OPEN' in line or 'PLAYER_FAIL' in line
                                            or 'IO ERROR' in line or 'SD_LEASE' in line
                                            or 'KSN_P5_REPAIR' in line or 'KSN_P5_LOWHEAP' in line
                                            or 'KSN_P5_NOTICE' in line or 'KSN_P5_MUSIC' in line
                                            or 'HELLO_COUNT' in line
                                            or 'KSN_OUTPUT_SOURCE' in line
                                            or 'KSN_P1_OUTPUT_OVERLAY' in line)],
        'error_count': sum('PLAYER_FAIL' in line or 'IO ERROR' in line for line in lines),
    }
    (args.out / 'summary.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2),
                                           encoding='utf-8')
    print('TRIAL_DONE', args.out, 'errors', summary['error_count'], flush=True)


if __name__ == '__main__':
    main()
