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
    args = parser.parse_args()
    if not args.removal_window_seconds and not args.track_cycles and (
            args.play_seconds < args.pause_after + 10 or args.pause_after < 5):
        parser.error('play-seconds must exceed pause-after by at least 10 seconds')
    if args.removal_window_seconds and args.removal_window_seconds < 20:
        parser.error('removal-window-seconds must be at least 20')
    if args.removal_paused and not args.removal_window_seconds:
        parser.error('removal-paused requires removal-window-seconds')
    if args.track_cycles < 0 or (args.track_cycles and args.removal_window_seconds):
        parser.error('track-cycles must be nonnegative and separate from removal test')
    if args.advance_tracks < 0 or (args.advance_tracks and
                                  (args.track_cycles or args.removal_window_seconds)):
        parser.error('advance-tracks must be nonnegative and used only in timed playback')
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
            collect(35, r'PLAYER_OPEN .*02 インザハウス\.mp3')
            for _ in range(args.advance_tracks):
                collect(0.5)
                key('b')
                collect(8, r'PLAYER_OPEN ')

            if args.track_cycles:
                for _ in range(args.track_cycles):
                    collect(0.5)
                    key('b')
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
                collect(args.pause_after)
                key('e')
                collect(8, r'transition=mp3_paused')
                collect(args.pause_seconds)
                key('e')
                collect(8, r'transition=mp3_playing')
                collect(args.play_seconds - args.pause_after)
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
        'marks': marks,
        'events': [line for line in lines if ('KSN_P0:' in line or 'MP3DEC' in line
                                            or 'PLAYER_OPEN' in line or 'PLAYER_FAIL' in line
                                            or 'IO ERROR' in line or 'SD_LEASE' in line)],
        'error_count': sum('PLAYER_FAIL' in line or 'IO ERROR' in line for line in lines),
    }
    (args.out / 'summary.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2),
                                           encoding='utf-8')
    print('TRIAL_DONE', args.out, 'errors', summary['error_count'], flush=True)


if __name__ == '__main__':
    main()
