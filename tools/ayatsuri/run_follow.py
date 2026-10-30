"""Build the pure follow kernel with strict numerical flags; no device operations."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
CACHE = ROOT / '.cache/ayatsuri-follow'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--m32', action='store_true', help='Linux SSE2 binary64 profile; no sanitizers')
    args = parser.parse_args()
    windows = sys.platform == 'win32'
    compiler = shutil.which('gcc') or shutil.which('cc')
    mingw = Path('C:/msys64/ucrt64/bin/gcc.exe')
    if windows and mingw.exists():
        compiler = str(mingw)
    if not compiler:
        raise SystemExit('A host C compiler is required')
    if windows and args.m32:
        parser.error('--m32 requires Linux and the existing sysroot helper')
    CACHE.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env['PATH'] = str(Path(compiler).parent) + os.pathsep + env.get('PATH', '')
    env.setdefault('ASAN_OPTIONS', 'detect_leaks=0:abort_on_error=1')
    env.setdefault('UBSAN_OPTIONS', 'halt_on_error=1:print_stacktrace=1')
    extra = []
    if args.m32:
        extra = subprocess.run(['bash', 'tools/vmtest/m32_sysroot.sh'], cwd=ROOT, env=env,
                               check=True, capture_output=True, text=True).stdout.split()
        extra += ['-msse2', '-mfpmath=sse']
    elif not windows:
        extra = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    previous = None
    for opt in ('0', '2'):
        label = ('m32' if args.m32 else 'mingw' if windows else 'asan') + '-O' + opt
        binary = CACHE / ('follow-' + label + ('.exe' if windows else ''))
        subprocess.run([compiler, '-std=c11', '-O' + opt, '-g', '-Wall', '-Wextra', '-Werror',
                        '-fno-fast-math', '-ffp-contract=off', '-I', 'main', *extra,
                        'tools/ayatsuri/test_follow.c', '-Wl,--wrap=malloc', '-Wl,--wrap=calloc',
                        '-Wl,--wrap=realloc', '-Wl,--wrap=free', '-lm', '-o', str(binary)],
                       cwd=ROOT, env=env, check=True)
        result = subprocess.run([str(binary), '--json'], cwd=ROOT, env=env, check=True,
                                stdout=subprocess.PIPE).stdout
        (CACHE / ('vectors-' + label + '.json')).write_bytes(result)
        if previous is not None and previous != result:
            raise SystemExit('O0/O2 vector output differs')
        previous = result
    (CACHE / 'vectors.json').write_bytes(previous)
    print('AYATSURI_FOLLOW_PROFILE_PASS O0/O2 exact; vectors:', CACHE / 'vectors.json')


if __name__ == '__main__':
    main()
