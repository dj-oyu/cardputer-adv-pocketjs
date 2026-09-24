"""Exercise audio-task producer -> pool -> mounted text slot diagnostics."""
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
    parser.add_argument('--probe', choices=('u', 'v', 'w', 'x', 'y', 'z', 'j'), default='v',
                        help='u=off; v=on; w=hide/show; x=pool exhaustion; y=full MP3/pause; z=dual text; j=seekable WAV')
    parser.add_argument('--capture', action='store_true',
                        help='capture live and ended LCD frames for v (w and z always capture)')
    parser.add_argument('--require-copy-watch', action='store_true',
                        help='require direct producer-pointer copy observations')
    parser.add_argument('--require-render-borrow', action='store_true',
                        help='require zero renderer text copies from a sealed core bank')
    args = parser.parse_args()
    if args.probe != 'v' and args.capture:
        parser.error('--capture is only for probe v; w and z capture automatically')
    if args.out.exists() and any(args.out.iterdir()):
        parser.error('--out must be new or empty')
    args.out.mkdir(parents=True, exist_ok=True)
    lines = []
    with (args.out / 'serial.log').open('w', encoding='utf-8') as log:
        port = serial.Serial(port=None, baudrate=115200, timeout=0.15)
        port.dtr = False
        port.rts = False
        port.port = args.port
        port.open()
        try:
            def read_line():
                line = port.readline().decode(errors='replace').strip()
                if line:
                    lines.append(line)
                    log.write(line + '\n')
                    log.flush()
                    if args.probe == 'y' and any(marker in line for marker in
                        ('KSN_OUTPUT_SOURCE TICK', 'KSN_OUTPUT_SOURCE PAUSED',
                         'KSN_OUTPUT_SOURCE RESUME_', 'KSN_OUTPUT_SOURCE STATE ended')):
                        print('PROGRESS', line, flush=True)
                return line

            def until(marker, seconds, seen_ok=False):
                if seen_ok:
                    for prior in lines:
                        if marker in prior:
                            return prior
                deadline = time.monotonic() + seconds
                while time.monotonic() < deadline:
                    line = read_line()
                    if marker in line:
                        print('SEEN', line, flush=True)
                        return line
                    if ('APP_FAILED' in line or 'KSN_OUTPUT_SOURCE ERROR' in line or
                            'panic' in line.lower()):
                        raise RuntimeError(line)
                raise TimeoutError(f'{marker}: {lines[-8:]}')

            def capture(name):
                port.write(b's')
                until('CAPTURE_BEGIN', 10)
                rows = {}
                deadline = time.monotonic() + 20
                while time.monotonic() < deadline:
                    line = read_line()
                    match = re.search(r'PIX (\d+) ([0-9a-f]{960})', line)
                    if match:
                        rows[int(match.group(1))] = bytes.fromhex(match.group(2))
                    if 'CAPTURE_END' in line:
                        break
                if len(rows) != 135:
                    raise RuntimeError(f'{name}: incomplete LCD capture ({len(rows)} rows)')
                pixels = b''.join(rows[y] for y in range(135))
                (args.out / f'{name}.rgb565').write_bytes(pixels)
                return pixels

            port.write(b'q')
            until('HOME_READY', 12)
            port.write(args.probe.encode('ascii'))
            until('KSN_OUTPUT_SOURCE BOUND', 15)
            if args.probe == 'j':
                until('KSN_OUTPUT_SOURCE CREATED bytes=22576', 30)
            else:
                until('pocket.sd: PICK 2 folders', 15)
                port.write(b'de')
                until('pocket.sd: GRANTED music', 15)
            until('KSN_OUTPUT_SOURCE OPEN', 15)
            until('KSN_OUTPUT_SOURCE PLAY', 15)
            live = hidden = shown = None
            if args.probe == 'w':
                until('KSN_OUTPUT_SOURCE HIDDEN', 20)
                hidden = capture('hidden')
                until('KSN_OUTPUT_SOURCE SHOWN', 30, seen_ok=True)
                time.sleep(0.5)
                shown = capture('shown')
            elif args.capture or args.probe == 'z':
                time.sleep(1)
                live = capture('live')
            until('KSN_OUTPUT_SOURCE CLOSED', 300 if args.probe == 'y' else 65,
                  seen_ok=True)
            time.sleep(0.5)
            ended = capture('ended') if args.capture or args.probe == 'z' else None
            port.write(b'q')
            stop = until('KSN_OUTPUT_SOURCE: STOP', 12) if args.probe != 'u' else None
            until('APP_STOPPED', 12)
            match = (re.search(r'published=(\d+) (?:valid_published=(\d+) )?'
                               r'skipped=(\d+) audio_stopped=(\d+)', stop)
                     if stop else None)
            numeric = (re.search(r'max_frames=(\d+) max_starved=(\d+)', stop)
                       if stop else None)
            if args.probe != 'u' and not match:
                raise RuntimeError(f'unexpected source stop line: {stop}')
            changed = outside = hidden_text = shown_text = outside_text = None
            left_changed = right_changed = dual_mismatch = None
            first, second = (hidden, shown) if args.probe == 'w' else (live, ended)
            if first is not None:
                changed = outside = 0
                if args.probe == 'z':
                    left_changed = right_changed = dual_mismatch = 0
                if args.probe == 'w':
                    hidden_text = shown_text = outside_text = 0
                    background = hidden[:2]
                for y in range(135):
                    for x in range(240):
                        at = 2 * (y * 240 + x)
                        in_text = 4 <= x < 92 and 4 <= y < 16
                        if args.probe == 'w' and in_text:
                            hidden_text += first[at:at + 2] != background
                            shown_text += second[at:at + 2] != background
                        if first[at:at + 2] != second[at:at + 2]:
                            changed += 1
                            if x >= (192 if args.probe == 'z' else 96) or y >= 24:
                                outside += 1
                            if args.probe == 'w' and not in_text:
                                outside_text += 1
                            if args.probe == 'z':
                                left_changed += 4 <= x < 92 and 4 <= y < 16
                                right_changed += 100 <= x < 188 and 4 <= y < 16
                        if args.probe == 'z' and 4 <= x < 92 and 4 <= y < 16:
                            other = 2 * (y * 240 + x + 96)
                            dual_mismatch += (first[at:at + 2] != first[other:other + 2] or
                                              second[at:at + 2] != second[other:other + 2])
            errors = [line for line in lines if 'APP_FAILED' in line or
                      'KSN_OUTPUT_SOURCE ERROR' in line or 'panic' in line.lower() or
                      'KSN_OUTPUT_SOURCE STATE error' in line or
                      'STREAM STARVED' in line or 'MP3 stop source_fault=1' in line or
                      'IO ERROR' in line or 'source stopped reading' in line]
            p0 = [line for line in lines if 'KSN_P0: A session=' in line]
            watched = [line for line in lines if 'KSN_P0: W session=app ' in line]
            watch_match = (re.search(r'source_text_core_calls=(\d+) bytes=(\d+) '
                                     r'length_mismatches=(\d+)', watched[-1])
                           if watched else None)
            core_text_logs = [line for line in lines if
                              'KSN_P0: C session=app kind=core_submit_text ' in line]
            core_text_match = (re.search(r'calls=(\d+) bytes=(\d+)', core_text_logs[-1])
                               if core_text_logs else None)
            copy_kinds = {}
            for line in lines:
                found = re.search(r'KSN_P0: C session=app kind=(\w+) calls=(\d+) bytes=(\d+)', line)
                if found:
                    copy_kinds[found.group(1)] = {'calls': int(found.group(2)),
                                                  'bytes': int(found.group(3))}
            pin_logs = [line for line in lines if 'KSN_OUTPUT_SOURCE: PIN_PROBE ' in line]
            pin_match = (re.search(r'pinned=(\d+) released=(\d+)', pin_logs[-1])
                         if pin_logs else None)
            final = [line for line in lines if 'KSN_OUTPUT_SOURCE FINAL' in line]
            final_match = re.search(r'underruns=(\d+)', final[-1]) if final else None
            position_match = re.search(r'positionMs=(\d+)', final[-1]) if final else None
            seek_accept_logs = [line for line in lines if 'KSN_OUTPUT_SOURCE SEEK_ACCEPT ' in line]
            seek_accept_match = (re.search(r'positionMs=(\d+) latencyMs=(\d+(?:\.\d+)?)',
                                           seek_accept_logs[-1]) if seek_accept_logs else None)
            seek_progress_logs = [line for line in lines if 'KSN_OUTPUT_SOURCE SEEK_PROGRESS ' in line]
            seek_progress_match = (re.search(r'positionMs=(\d+) latencyMs=(\d+(?:\.\d+)?)',
                                             seek_progress_logs[-1]) if seek_progress_logs else None)
            decoders = [line for line in lines if 'MP3DEC packets=' in line]
            decoder_match = re.search(r'faults=(\d+)', decoders[-1]) if decoders else None
            def cycle_values(pattern, convert=int):
                values = {}
                for line in lines:
                    found = re.search(pattern, line)
                    if found:
                        values[int(found.group(1))] = convert(found.group(2))
                return values
            paused_positions = cycle_values(r'KSN_OUTPUT_SOURCE PAUSED cycle=(\d+) positionMs=(\d+)')
            resume_positions = cycle_values(r'KSN_OUTPUT_SOURCE RESUME_REQUEST cycle=(\d+) positionMs=(\d+)')
            resume_accept = cycle_values(
                r'KSN_OUTPUT_SOURCE RESUME_ACCEPT cycle=(\d+) latencyMs=(\d+(?:\.\d+)?)', float)
            resume_progress = cycle_values(
                r'KSN_OUTPUT_SOURCE RESUME_PROGRESS cycle=(\d+) latencyMs=(\d+(?:\.\d+)?)', float)
            pause_drift = {cycle: resume_positions[cycle] - value
                           for cycle, value in paused_positions.items()
                           if cycle in resume_positions}
            metrics = {}
            for line in lines:
                found = re.search(r'KSN_P0: S session=app metric=(\w+).*?p95=(\d+) '
                                  r'p99=(\d+) max=(\d+) over12=(\d+)', line)
                if found:
                    metrics[found.group(1)] = {'p95': int(found.group(2)),
                                               'p99': int(found.group(3)),
                                               'max': int(found.group(4)),
                                               'over12': int(found.group(5))}
            summary = {'binary_probe': args.probe,
                       'capture': args.capture or args.probe in ('w', 'z'),
                       'published': int(match.group(1)) if match else None,
                       'valid_published': int(match.group(2)) if match and match.group(2) else None,
                       'skipped': int(match.group(3)) if match else None,
                       'audio_stopped': int(match.group(4)) if match else None,
                       'source_text_core_calls': int(watch_match.group(1)) if watch_match else None,
                       'source_text_core_bytes': int(watch_match.group(2)) if watch_match else None,
                       'source_text_length_mismatches': int(watch_match.group(3)) if watch_match else None,
                       'core_submit_text_calls': int(core_text_match.group(1)) if core_text_match else None,
                       'core_submit_text_bytes': int(core_text_match.group(2)) if core_text_match else None,
                       'copy_kinds': copy_kinds,
                       'probe_pinned': int(pin_match.group(1)) if pin_match else None,
                       'probe_released': int(pin_match.group(2)) if pin_match else None,
                       'max_published_frames': int(numeric.group(1)) if numeric else None,
                       'max_starved_blocks': int(numeric.group(2)) if numeric else None,
                       'changed_pixels': changed,
                       'outside_source_pixels': outside,
                       'hidden_text_pixels': hidden_text,
                       'shown_text_pixels': shown_text,
                       'outside_text_pixels': outside_text,
                       'left_changed_pixels': left_changed,
                       'right_changed_pixels': right_changed,
                       'dual_text_mismatch_pixels': dual_mismatch,
                       'audio_log': p0, 'final_log': final,
                       'final_underruns': int(final_match.group(1)) if final_match else None,
                       'final_position_ms': int(position_match.group(1)) if position_match else None,
                       'seek_accept_position_ms': int(seek_accept_match.group(1)) if seek_accept_match else None,
                       'seek_accept_latency_ms': float(seek_accept_match.group(2)) if seek_accept_match else None,
                       'seek_progress_position_ms': int(seek_progress_match.group(1)) if seek_progress_match else None,
                       'seek_progress_latency_ms': float(seek_progress_match.group(2)) if seek_progress_match else None,
                       'seek_probe_removed': any('KSN_OUTPUT_SOURCE REMOVED' in line for line in lines),
                       'decoder_faults': int(decoder_match.group(1)) if decoder_match else None,
                       'seek_unsupported': any('KSN_OUTPUT_SOURCE SEEK_UNSUPPORTED code=NOT_AVAILABLE'
                                               in line for line in lines),
                       'ended_state': any('KSN_OUTPUT_SOURCE STATE ended' in line for line in lines),
                       'paused_positions': paused_positions,
                       'resume_positions': resume_positions,
                       'resume_accept_ms': resume_accept,
                       'resume_progress_ms': resume_progress,
                       'pause_drift_ms': pause_drift,
                       'metrics': metrics, 'errors': errors}
            (args.out / 'summary.json').write_text(json.dumps(summary, indent=2),
                                                   encoding='utf-8')
            if ((args.probe != 'u' and
                    (summary['published'] < 3 or summary['audio_stopped'] != 1 or
                     summary['max_published_frames'] is None or
                     summary['max_published_frames'] <= (24000 if args.probe == 'j' else 65535) or
                     summary['max_starved_blocks'] != 0 or
                     (args.probe == 'x' and
                      (summary['skipped'] == 0 or summary['probe_pinned'] != 2 or
                       summary['probe_released'] != 2)) or
                     (args.probe != 'x' and summary['skipped'] != 0))) or
                    summary['final_underruns'] != 0 or
                    (summary['decoder_faults'] != 0 if args.probe != 'j' else
                     summary['decoder_faults'] not in (None, 0)) or errors or
                    'app_render' not in metrics or 'app_send' not in metrics or
                    (args.require_copy_watch and
                     (watch_match is None or summary['source_text_core_calls'] == 0 or
                      summary['source_text_core_bytes'] != 8 * summary['source_text_core_calls'] or
                      summary['source_text_length_mismatches'] != 0)) or
                    (args.require_render_borrow and
                     (not args.require_copy_watch or
                      any(copy_kinds.get(kind, {}).get('calls', 0) != 0
                          for kind in ('core_render_text', 'render_decode_text')))) or
                    (args.probe == 'w' and
                     (summary['valid_published'] is None or
                      summary['source_text_core_calls'] is None or
                      summary['source_text_core_calls'] >= summary['valid_published'] or
                      hidden_text != 0 or shown_text == 0 or outside_text != 0)) or
                    (args.probe == 'z' and
                     (summary['valid_published'] is None or
                      summary['source_text_core_calls'] is None or
                      summary['source_text_core_calls'] < 2 or
                      summary['source_text_core_calls'] % 2 != 0 or
                      summary['source_text_core_calls'] > 2 * summary['valid_published'] or
                      summary['core_submit_text_calls'] is None or
                      summary['core_submit_text_calls'] < summary['source_text_core_calls'] or
                      left_changed == 0 or right_changed == 0 or
                      left_changed != right_changed or dual_mismatch != 0)) or
                    (args.probe == 'j' and
                     (not any('KSN_OUTPUT_SOURCE OPEN codec=wav/ima-adpcm ' in line and
                              'seekable=true' in line for line in lines) or
                      not summary['seek_probe_removed'] or not summary['ended_state'] or
                      summary['seek_accept_position_ms'] is None or
                      not 900 <= summary['seek_accept_position_ms'] <= 1300 or
                      summary['seek_accept_latency_ms'] is None or
                      summary['seek_accept_latency_ms'] > 1000 or
                      summary['seek_progress_position_ms'] is None or
                      not 1200 <= summary['seek_progress_position_ms'] <= 1900 or
                      summary['seek_progress_latency_ms'] is None or
                      summary['seek_progress_latency_ms'] > 1500 or
                      summary['final_position_ms'] is None or
                      not 1800 <= summary['final_position_ms'] <= 1900)) or
                    (args.probe == 'y' and
                     (not summary['seek_unsupported'] or not summary['ended_state'] or
                      summary['final_position_ms'] is None or
                      summary['final_position_ms'] < 220000 or
                      set(paused_positions) != {0, 1} or
                      set(resume_positions) != {0, 1} or
                      set(resume_accept) != {0, 1} or
                      set(resume_progress) != {0, 1} or
                      set(pause_drift) != {0, 1} or
                      any(not 0 <= drift <= 100 for drift in pause_drift.values()) or
                      any(ms > 1000 for ms in resume_accept.values()) or
                      any(ms > 2000 for ms in resume_progress.values()))) or
                    (changed is not None and (changed == 0 or outside != 0))):
                raise RuntimeError(f'output-source gate: {summary}')
            print('OUTPUT_SOURCE PASS', summary, flush=True)
        finally:
            port.close()


if __name__ == '__main__':
    main()
