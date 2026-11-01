"""Build the shared, allocation-free ayatsuri C transform contract; host only.

Linux runs O0/O2 with ASan/UBSan. Windows uses the installed MinGW compiler
without sanitizers. Both write the same deterministic 408-case JSON schema
for an independently implemented JS oracle. No SDK, device or flash access.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / '.cache/ayatsuri-transform'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--m32', action='store_true', help='32-bit SSE2 host profile, no sanitizers')
    parser.add_argument('--x87-diagnostic', action='store_true', help='also compare default x87, without guaranteeing parity')
    args = parser.parse_args()
    compiler = shutil.which('gcc') or shutil.which('cc')
    mingw = Path('C:/msys64/ucrt64/bin/gcc.exe')
    if sys.platform == 'win32' and mingw.exists():
        compiler = str(mingw)
    if not compiler:
        raise SystemExit('A host C compiler is required')
    windows = sys.platform == 'win32'
    if args.m32 and windows:
        parser.error('--m32 uses the Linux sysroot helper')
    if args.x87_diagnostic and not args.m32:
        parser.error('--x87-diagnostic requires --m32')
    OUT.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env['PATH'] = str(Path(compiler).parent) + os.pathsep + env.get('PATH', '')
    env.setdefault('ASAN_OPTIONS', 'detect_leaks=0:abort_on_error=1')
    env.setdefault('UBSAN_OPTIONS', 'halt_on_error=1:print_stacktrace=1')
    m32_flags = []
    if args.m32:
        m32_flags = subprocess.run(['bash', 'tools/vmtest/m32_sysroot.sh'],
                                   cwd=ROOT, env=env, check=True, capture_output=True,
                                   text=True).stdout.split()

    def run(opt, diagnostic=False):
        label = ('m32-x87' if diagnostic else 'm32-sse' if args.m32 else
                 'mingw' if windows else 'asan') + '-O' + opt
        binary = OUT / ('test-transform-' + label + ('.exe' if windows else ''))
        flags = ['-std=c11', '-O' + opt, '-g', '-Wall', '-Wextra', '-Werror',
                 '-ffp-contract=off', '-fno-fast-math', '-I', 'main']
        if not diagnostic:
            flags += ['-DAYATSURI_REQUIRE_BINARY64=1']
        if args.m32:
            flags += m32_flags
            if not diagnostic:
                flags += ['-msse2', '-mfpmath=sse']
        elif not windows:
            flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run([compiler, *flags, 'tools/ayatsuri/test_transform.c',
                        '-lm', '-o', str(binary.relative_to(ROOT))], cwd=ROOT,
                       env=env, check=True)
        result = subprocess.run([str(binary), '--json'], cwd=ROOT, env=env,
                                stdout=subprocess.PIPE, check=True).stdout
        vectors = json.loads(result)
        if len(vectors) != 408:
            raise SystemExit('Unexpected vector count')
        (OUT / ('vectors-' + label + '.json')).write_bytes(result)
        print('AYATSURI_C ' + label + ' PASS: 408 vectors', flush=True)
        return result

    results = [run(opt) for opt in ('0', '2')]
    if results[0] != results[1]:
        raise SystemExit('O0/O2 vectors differ')
    (OUT / 'vectors.json').write_bytes(results[-1])
    print('AYATSURI_C O0/O2 exact; wrote .cache/ayatsuri-transform/vectors.json')
    if args.x87_diagnostic:
        diagnostic = json.loads(run('2', diagnostic=True))
        reference = json.loads(results[-1])
        changed_input = changed_status = changed_component = 0
        def bits(row):
            return None if row is None else b''.join(struct.pack('f', x) for x in row)
        for a, b in zip(reference, diagnostic):
            changed_input += bits(a['pose']) != bits(b['pose']) or bits(a['parent']) != bits(b['parent'])
            changed_status += a['status'] != b['status']
            if a['output'] is not None and b['output'] is not None:
                changed_component += sum(struct.pack('f', x) != struct.pack('f', y)
                                         for x, y in zip(a['output'], b['output']))
        print(f'AYATSURI_C x87 diagnostic only: input_differences={changed_input} status_differences={changed_status} '
              f'component_bit_differences={changed_component}; not a general parity guarantee')


if __name__ == '__main__':
    main()
