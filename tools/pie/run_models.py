"""Build and run the exhaustive C models in models/ with whatever host C
compiler is available (gcc, clang, cc, or `zig cc`).

    python tools/pie/run_models.py            every model
    python tools/pie/run_models.py ocean      one of: ocean, wave, garden,
                                              fir, canopy, disc, blendpack, scale256

Each model prints its own verdict; this script fails if any of them reports a
mismatch or a non-zero exit. The ocean and blendpack models need a few seconds,
the others about a second. scale256 is the one whose sweep is expected to find
moved pixels (it is an approximation being measured, not an identity being
proved): it prints those counts and still exits 0, because the claims it does
assert -- the scale's identities, the off arm against the pre-change formula,
and the alphas that must not move -- all hold.
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
    'garden': (['garden_model.c'], []),
    'fir': (['fir_model.c'], []),
    'canopy': (['canopy_model.c'], []),
    'disc': (['disc_model.c'], []),
    'blendpack': (['blend_pack_model.c'], []),
    'scale256': (['scale256_model.c'], []),
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
