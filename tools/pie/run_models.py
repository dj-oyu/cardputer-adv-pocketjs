"""Build and run the exhaustive C models in models/ with whatever host C
compiler is available (gcc, clang, cc, or `zig cc`).

    python tools/pie/run_models.py            all four
    python tools/pie/run_models.py blend      one of: ocean, wave, blend, accel, garden

Each model prints its own verdict; this script fails if any of them reports a
mismatch or a non-zero exit. The ocean model needs a few seconds, the others
about a second.
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
MODELS = os.path.join(HERE, 'models')
TARGETS = {
    'ocean': (['ocean_model.c'], []),
    'wave': (['wave_model.c'], []),
    'blend': (['blend_model.c'], []),
    'accel': (['accel_host_test.c'], ['-DRENDER_ACCEL_HOST_MODEL', '-I' + os.path.join(MODELS, 'stub')]),
    'garden': (['garden_model.c'], []),
}


def compiler():
    for cand in (['gcc'], ['clang'], ['cc'], ['zig', 'cc']):
        if shutil.which(cand[0]):
            return cand
    sys.exit('no host C compiler found (gcc, clang, cc or zig)')


def main():
    names = sys.argv[1:] or list(TARGETS)
    cc = compiler()
    failed = []
    with tempfile.TemporaryDirectory() as tmp:
        for name in names:
            srcs, flags = TARGETS[name]
            exe = os.path.join(tmp, name + ('.exe' if os.name == 'nt' else ''))
            cmd = cc + ['-O2', '-o', exe] + [os.path.join(MODELS, s) for s in srcs] + flags + ['-lm']
            print('$', ' '.join(cmd), flush=True)
            if subprocess.call(cmd) != 0:
                failed.append(name)
                continue
            out = subprocess.run([exe], capture_output=True, text=True)
            print(out.stdout.rstrip())
            if out.returncode != 0 or 'mismatches=0' not in out.stdout.replace(' ', '') and 'mismatches 0' not in out.stdout:
                failed.append(name)
    if failed:
        sys.exit('FAILED: ' + ', '.join(failed))
    print('all models agree')


if __name__ == '__main__':
    main()
