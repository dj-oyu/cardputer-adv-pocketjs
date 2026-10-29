"""Actual QuickJS, native shapes and Kasane band presentation, host only."""
import argparse
import os
from pathlib import Path
import subprocess
from run_megademo_app_host import ROOT, build


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--frames-per-mode', type=int, default=301)
    parser.add_argument('--ppm', type=Path)
    parser.add_argument('--m32', action='store_true', help='device-sized JSValue/pointers, no sanitizers')
    args = parser.parse_args()
    if args.frames_per_mode < 2:
        parser.error('--frames-per-mode must be at least 2 to release mode-switch keys')
    flags = ['-fsanitize=address,undefined']
    cache = ROOT / '.cache/character-native'
    if args.m32:
        cache = ROOT / '.cache/character-native-m32'
        sysroot = dict(os.environ, M32_SYSROOT=os.environ.get('M32_SYSROOT', str(cache / 'm32sys')))
        flags = subprocess.run(['bash','tools/vmtest/m32_sysroot.sh'],cwd=ROOT,env=sysroot,
                               check=True,capture_output=True,text=True).stdout.split()
    binary = build(flags, cache,
                   'tools/kasane_contract/test_character_app_host.c')
    env = os.environ.copy()
    env.setdefault('ASAN_OPTIONS', 'detect_leaks=0:abort_on_error=1')
    env.setdefault('UBSAN_OPTIONS', 'halt_on_error=1:print_stacktrace=1')
    if args.ppm:
        args.ppm.mkdir(parents=True, exist_ok=True)
        env['MEGA_PPM'] = str(args.ppm.resolve())
    subprocess.run([str(binary),str(args.frames_per_mode)],cwd=ROOT,env=env,check=True)


if __name__ == '__main__':
    main()
