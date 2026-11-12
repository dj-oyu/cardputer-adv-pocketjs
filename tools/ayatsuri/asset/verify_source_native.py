"""Synthetic planar source/reduced images through current C model + mesh renderer."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[3]
ASSET = ROOT / 'tools/ayatsuri/asset'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def compare(a, b):
    if len(a) != 240 * 135 * 2 or len(b) != len(a) or a != b:
        raise ValueError('NATIVE_PIXEL_MISMATCH')
    values = struct.unpack('<32400H', a)
    coverage = sum(v != 0 for v in values)
    if not coverage:
        raise ValueError('EMPTY_RENDER')
    return coverage


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output-dir', type=Path, required=True)
    p.add_argument('--asan', action='store_true')
    args = p.parse_args()
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=False)
    sources = [ASSET / 'render_source_probe.c'] + [ROOT / 'main' / x for x in (
        'ayatsuri/ayatsuri_model.c', 'ayatsuri/ayatsuri_visibility.c',
        'ayatsuri/compact_opaque.c', 'ui/kasane/ksn_shiki_frame.c',
        'ui/kasane/ksn_mesh_depth.c')]
    dependencies = sorted(set(sources + list((ROOT / 'main/ayatsuri').glob('*.h')) +
                              list((ROOT / 'main/ui/kasane').glob('*.h')) +
                              list(ASSET.glob('*.cjs')) +
                              list((ASSET / 'compact_opaque').glob('*.cjs')) +
                              [ASSET.parent / 'model/pack_model.cjs', Path(__file__)]))
    before = {str(f.relative_to(ROOT)): digest(f) for f in dependencies}
    report = {'scope': 'SYNTHETIC PLANAR HOST REGRESSION, NOT FOX/BLENDER/DEVICE',
              'passed': False, 'cases': [], 'sourceSha256': before,
              'sanitizers': 'ASan/UBSan; LSan disabled' if args.asan else 'none'}
    env = dict(os.environ)
    if args.asan:
        env.update(ASAN_OPTIONS='detect_leaks=0:abort_on_error=1', UBSAN_OPTIONS='halt_on_error=1')
    def run(command, label):
        proc = subprocess.run(list(map(str, command)), cwd=ROOT, env=env,
                              text=True, capture_output=True, timeout=120)
        (out / (label + '.log')).write_text(proc.stdout + proc.stderr)
        proc.check_returncode()
    try:
        run(['node', ASSET / 'test_reduce.cjs', out], 'fixtures')
        inputs = {name: digest(out / (name + '.aynm')) for name in ('identity', 'reduced')}
        flags = ['-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-ffp-contract=off']
        if args.asan:
            flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        run(['cc', *flags, '-I', ROOT / 'main', *sources, '-lm', '-o', out / 'render'], 'compile')
        report['lineageSha256'] = digest(out / 'lineage.json')
        report['sourceIRSha256'] = digest(out / 'source.ir.json')
        report['reducedIRSha256'] = digest(out / 'reduced.ir.json')
        report.update(flags=flags, compiler=subprocess.check_output(['cc', '--version'], text=True).splitlines()[0], modelSha256=inputs)
        for yaw in (-35, 0, 35):
            paths = [out / f'{name}-{yaw}.u16le' for name in ('identity', 'reduced')]
            for name, target in zip(('identity', 'reduced'), paths):
                run([out / 'render', out / (name + '.aynm'), target, yaw], f'render-{name}-{yaw}')
            a, b = [f.read_bytes() for f in paths]
            coverage = compare(a, b)
            if yaw == 0:
                values = struct.unpack('<32400H', a)
                expected = [80 <= x < 160 and 27 <= y < 107 for y in range(135) for x in range(240)]
                if [v != 0 for v in values] != expected:
                    raise ValueError('ANALYTIC_PLANE_COVERAGE_MISMATCH')
            changed = bytearray(b)
            changed[0] ^= 1
            try:
                compare(a, changed)
            except ValueError:
                pass
            else:
                raise AssertionError('negative mutation was not detected')
            report['cases'].append({'yaw': yaw, 'coveragePixels': coverage, 'exactRGB565': True,
                                    'imageSha256': digest(paths[0])})
        assert inputs == {name: digest(out / (name + '.aynm')) for name in inputs}
        assert before == {str(f.relative_to(ROOT)): digest(f) for f in dependencies}
        report['passed'] = True
        report['limitations'] = ['Only planar uniform-normal synthetic geometry',
                                'RGB equality is a differential regression, not an independent shader oracle',
                                'Curved quantization, occlusion semantics, Blender/fox art and device not accepted']
    finally:
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'passed': report['passed'], 'cases': report['cases'], 'report': str(out / 'report.json')}))


if __name__ == '__main__':
    main()
