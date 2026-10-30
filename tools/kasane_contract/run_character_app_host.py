"""Actual QuickJS, native shapes and Kasane band presentation, host only."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
from run_megademo_app_host import ROOT, build


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cloud', action='store_true', help='run the dedicated one-model CLOUD ACT app')
    parser.add_argument('--fixture', action='store_true', help='tap Enter for the bounded cloud fixture twice, then verify U return and deterministic logs')
    parser.add_argument('--fixture-max-frames', type=int, default=360, help='fixture button-frame budget, 1..360')
    parser.add_argument('--frames-per-mode', type=int, default=301)
    parser.add_argument('--ppm', type=Path)
    parser.add_argument('--m32', action='store_true', help='device-sized JSValue/pointers, no sanitizers')
    parser.add_argument('--capture-dir', type=Path, help='host-only diagnostic images and provenance manifest; separate from the 5418-frame regression')
    parser.add_argument('--capture-ticks', help='override default per-mode ticks: comma list or inclusive start:stop[:step], including reverse ranges')
    args = parser.parse_args()
    if args.fixture and (not args.cloud or args.capture_dir or args.ppm):
        parser.error('--fixture requires --cloud and cannot be combined with capture/PPM output')
    if not 1 <= args.fixture_max_frames <= 360:
        parser.error('--fixture-max-frames must be in 1..360')
    if args.capture_ticks and not args.capture_dir:
        parser.error('--capture-ticks requires --capture-dir')
    sys.path.insert(0, str(ROOT / 'tools/character'))
    import capture_character as capture
    try:
        selected_ticks = capture.ticks(args.capture_ticks) if args.capture_ticks else None
    except ValueError as error:
        parser.error(str(error))
    if args.frames_per_mode < 2:
        parser.error('--frames-per-mode must be at least 2 to release mode-switch keys')
    flags = ['-fsanitize=address,undefined']
    cache = ROOT / ('.cache/character-fixture-asan' if args.fixture else '.cache/character-native')
    if args.m32:
        cache = ROOT / ('.cache/character-fixture-m32' if args.fixture else '.cache/character-native-m32')
        sysroot = dict(os.environ, M32_SYSROOT=os.environ.get('M32_SYSROOT', str(cache / 'm32sys')))
        flags = subprocess.run(['bash','tools/vmtest/m32_sysroot.sh'],cwd=ROOT,env=sysroot,
                               check=True,capture_output=True,text=True).stdout.split()
        # Shared TRS intermediates are binary64, as on S3 and JS Number.
        # i386 x87's excess precision is a different numerical profile.
        flags += ['-msse2', '-mfpmath=sse']
    binary = build(flags, cache,
                   'tools/kasane_contract/test_character_app_host.c')
    env = os.environ.copy()
    entry = 'apps/cloudprobe/cloud_probe.js' if args.fixture else 'apps/cloud/cloud_act.js' if args.cloud else 'apps/character/character_lab.js'
    chunks = 'apps/cloudprobe/chunks.txt' if args.fixture else 'apps/cloud/chunks.txt' if args.cloud else 'apps/character/chunks.txt'
    models = ('cloud',) if args.cloud else capture.MODELS
    env['CHARACTER_APP_ENTRY'] = entry
    env['CHARACTER_APP_CHUNKS'] = chunks
    env.setdefault('ASAN_OPTIONS', 'detect_leaks=0:abort_on_error=1')
    env.setdefault('UBSAN_OPTIONS', 'halt_on_error=1:print_stacktrace=1')
    if args.fixture:
        env.update(CHARACTER_FIXTURE='1', CHARACTER_FIXTURE_MAX_FRAMES=str(args.fixture_max_frames),
                   CHARACTER_HEAP_LIMIT='163840', CHARACTER_NO_EAGER_GC='1', CHARACTER_FULL_REFERENCE='1')
        for key in ('CHARACTER_CAPTURE_DIR', 'CHARACTER_CAPTURE_PLAN'):
            env.pop(key, None)
        runs = []
        for attempt in (1, 2):
            result = subprocess.run([str(binary)], cwd=ROOT, env=env, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            (cache / f'fixture-run-{attempt}.log').write_text(result.stdout)
            print(result.stdout, end='')
            protocol = [line.strip() for line in result.stdout.splitlines() if line.strip().startswith('CLOUD_FIXTURE ')]
            panels = [line.strip() for line in result.stdout.splitlines() if line.startswith('CHARACTER_FIXTURE_PANEL ')]
            runs.append(dict(returncode=result.returncode,
                             passed=result.returncode == 0 and 'CHARACTER_FIXTURE_PASS' in result.stdout,
                             fixture_log=protocol, panel_log=panels))
        deterministic = bool(runs[0]['fixture_log']) and runs[0]['fixture_log'] == runs[1]['fixture_log'] and runs[0]['panel_log'] == runs[1]['panel_log']
        passed = all(run['passed'] for run in runs) and deterministic
        proof = dict(kind='HOST SIMULATION', passed=passed, deterministic=deterministic,
                     requested_runs=2, max_fixture_frames=args.fixture_max_frames,
                     heap_limit_bytes=163840, per_frame_forced_gc=False, full_reference_every_frame=True,
                     provenance=capture.provenance(ROOT, binary, flags), runs=runs)
        (cache / 'fixture-result.json').write_text(json.dumps(proof, indent=2) + '\n')
        print('CHARACTER_FIXTURE_REPEAT_' + ('PASS' if passed else 'FAIL'), cache / 'fixture-result.json')
        if not passed:
            raise SystemExit(1)
        return
    if args.ppm:
        args.ppm.mkdir(parents=True, exist_ok=True)
        env['MEGA_PPM'] = str(args.ppm.resolve())
    if args.capture_dir:
        folder = args.capture_dir.resolve()
        plan, records = capture.prepare(folder, selected_ticks, models=models)
        env['CHARACTER_CAPTURE_DIR'] = str(folder)
        env['CHARACTER_CAPTURE_PLAN'] = str(plan)
        source = capture.provenance(ROOT, binary, flags)
        source.update(app_entry=entry, app_chunks=chunks, model_names=models)
        run = subprocess.run([str(binary), str(args.frames_per_mode)], cwd=ROOT, env=env,
                             text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        print(run.stdout, end='')
        passed = run.returncode == 0 and 'CHARACTER_CAPTURE_PASS' in run.stdout
        print('Wrote', capture.finish(folder, records, source, passed, run.stdout))
        if not passed:
            raise SystemExit(run.returncode or 1)
    else:
        subprocess.run([str(binary),str(args.frames_per_mode)],cwd=ROOT,env=env,check=True)


if __name__ == '__main__':
    main()
