"""Bounded synthetic source-to-native image study. No Blender, SDK or device."""
from __future__ import annotations
import argparse
import hashlib
import json
import math
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
ASSET = ROOT / 'tools/ayatsuri/asset'


def sha(file):
    return hashlib.sha256(file.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--asan', action='store_true')
    args = parser.parse_args()
    output = args.output_dir.resolve()
    if output.exists():
        parser.error('choose a new output directory; prior evidence is not replaced')
    output.mkdir(parents=True)
    result = {'scope': 'SYNTHETIC HOST NATIVE IMAGE STUDY; NOT BLENDER / FOX ART / DEVICE', 'passed': False, 'runs': []}
    names = [*sorted(ASSET.glob('*.cjs')), *sorted(ASSET.glob('*.py')), *sorted(ASSET.glob('*.c')),
             ROOT / 'tools/ayatsuri/model/pack_model.cjs', ROOT / 'tools/ayatsuri/model/export_cloud.cjs',
             *sorted((ROOT / 'main/ayatsuri').glob('*.h')), ROOT / 'main/ayatsuri/ayatsuri_model.c',
             ROOT / 'main/ayatsuri/ayatsuri_visibility.c', ROOT / 'main/ui/kasane/ksn_procedural.c',
             ROOT / 'main/ui/kasane/ksn_procedural.h']
    before = {str(p.relative_to(ROOT)): sha(p) for p in names}
    def run(command, label, expected=0):
        proc = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=180)
        (output / (label + '.stdout.txt')).write_text(proc.stdout)
        (output / (label + '.stderr.txt')).write_text(proc.stderr)
        result['runs'].append({'label': label, 'exit': proc.returncode, 'expected': expected})
        allowed = expected if isinstance(expected, (tuple, list)) else [expected]
        if proc.returncode not in allowed:
            raise RuntimeError(f'{label}: exit {proc.returncode} != {expected}: {proc.stderr[-2000:]}')
        return proc
    try:
        node = shutil.which('node')
        if not node:
            raise RuntimeError('existing Node is required; no software setup is invoked')
        run([node, str(ASSET / 'test_import.cjs')], 'import')
        synthetic = output / 'synthetic'
        run([node, str(ASSET / 'test_pipeline.cjs'), str(synthetic)], 'pipeline')
        thresholds = {'minIoU': .97, 'maxColorMean': .5, 'maxColorP95': 1, 'maxColorMax': 2,
                      'maxDepthSource': .01, 'maxComponentMismatch': 0,
                      'maxCorrespondenceDistanceSource': .01, 'depthTieEpsilonSource': 1e-6}
        threshold_file = output / 'thresholds.json'
        threshold_file.write_text(json.dumps(thresholds, indent=2) + '\n')
        identity_profile = json.loads((synthetic / 'curved-profile.json').read_text())
        identity_profile['targetVertices'] = 81
        (output / 'identity-profile.json').write_text(json.dumps(identity_profile) + '\n')
        for name, profile in [('reduced', synthetic / 'curved-profile.json'), ('identity', output / 'identity-profile.json')]:
            run([node, str(ASSET / 'pipeline.cjs'), str(synthetic / 'curved-source.glb'), str(profile), str(output / name)], 'compile-' + name)
        def render(model, target, camera, light, pose=None):
            command = [sys.executable, str(ASSET / 'render_native.py'), str(model), '--output-dir', str(target),
                       '--camera', *map(str, camera), '--light', *map(str, light)]
            if pose is not None:
                command += ['--root-pose', *map(str, pose)]
            if args.asan:
                command += ['--asan']
            run(command, 'render-' + target.name)
        summaries = []
        result['cases'] = summaries
        front = [0, 0, 400, 0, 0, 0, 1, 120, 67]
        light = [-.5, -.4, .8, 1, 1, 1]
        cases = [('front', front, light, None)]
        for yaw in [-35, 35]:
            a = math.radians(yaw)
            camera = [400 * math.sin(a), 0, 400 * math.cos(a), 0, 0, 0, 1, 120, 67]
            cases.append((f'yaw{yaw}', camera, light, None))
        cases += [('light', front, [.6, -.2, .75, 1, 1, 1], None),
                  ('body', front, light, [0, 0, 0, 0, .2, 0, 1, 1, 1]),
                  ('replay', front, light, None)]
        def evaluate(name, asset, camera, lighting, pose):
            native = output / name
            render(output / asset / 'candidate.aynm', native, camera, lighting, pose)
            oracle_dir = output / (name + '-oracle')
            proc = run([node, str(ASSET / 'oracle.cjs'), str(output / asset / 'scene.ir.json'), str(output / asset / 'report.json'),
                 str(native), str(threshold_file), str(oracle_dir)], 'oracle-' + name, expected=(0, 1))
            if not (oracle_dir / 'oracle.json').is_file():
                raise RuntimeError(f'oracle-{name} aborted: {proc.stderr[-2000:]}')
            report = json.loads((oracle_dir / 'oracle.json').read_text())
            summaries.append({'case': name, 'passed': report['passed'], 'gates': report['gates'], 'metrics': report['metrics']})
            return report['passed']
        if not evaluate('identity-front', 'identity', front, light, None):
            raise RuntimeError('identity source model failed native image verification')
        candidate_results = []
        selected = None
        for target in [49, 65]:
            asset = 'reduced' if target == 49 else 'candidate65'
            if target == 65:
                profile = json.loads((synthetic / 'curved-profile.json').read_text())
                profile['targetVertices'] = 65
                profile_file = output / 'candidate65-profile.json'
                profile_file.write_text(json.dumps(profile) + '\n')
                run([node, str(ASSET / 'pipeline.cjs'), str(synthetic / 'curved-source.glb'), str(profile_file), str(output / asset)], 'compile-' + asset)
            accepted = True
            for suffix, camera, lighting, pose in cases:
                accepted = evaluate(f'v{target}-' + suffix, asset, camera, lighting, pose) and accepted
            for image in ['pixels.u16le', 'visible-triangles.u16le', 'coverage.u16le']:
                if (output / f'v{target}-front' / image).read_bytes() != (output / f'v{target}-replay' / image).read_bytes():
                    raise RuntimeError('native deterministic replay mismatch: ' + image)
            compiled = json.loads((output / asset / 'report.json').read_text())
            candidate_results.append({'targetVertices': target, 'passedSampledNativeDomain': accepted,
                                      'coldBytes': compiled['coldBytes'], 'arenaEstimate': compiled['arenaConservativeEstimate'],
                                      'segments': compiled['segmentsUpperBound'], 'binSha256': compiled['binSha256']})
            if accepted:
                selected = target
                break
        result['candidateEvaluations'] = candidate_results
        result['selectedSyntheticCandidate'] = selected
        if selected is None:
            raise RuntimeError('no candidate met unchanged native image/resource gates')
        fixture_code = "const f=require('./tools/ayatsuri/asset/test_native_pixels.cjs');const k=process.argv[1],d=process.argv[2],x=k==='same-surface'?f.sameSurfaceFixtures():f.overlapFixtures();(k==='same-surface'?f.checkSameSurfaceInvariants:f.checkFixtureInvariants)(x);f.writeFixtures(d,x);"
        negative_results = []
        for kind in ['two-surfaces', 'same-surface']:
            fixture_dir = output / kind
            run([node, '-e', fixture_code, kind, str(fixture_dir)], 'fixture-' + kind)
            for mode in ['correct', 'reversed']:
                render(fixture_dir / (mode + '.aynm'), output / (kind + '-' + mode), front, light)
            proc = run([node, str(ASSET / 'test_native_pixels.cjs'), kind, str(output / (kind + '-correct')),
                        str(output / (kind + '-reversed')), str(output / (kind + '-oracle'))], 'negative-' + kind)
            negative_results.append({'case': kind, 'stdout': proc.stdout.strip()})
        result.update(passed=True, negativeControls=negative_results, replayEqual=True)
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        result['error'] = str(error)
    result['sourcesUnchanged'] = before == {str(p.relative_to(ROOT)): sha(p) for p in names}
    result['sourceSha256'] = before
    result['passed'] = result['passed'] and result['sourcesUnchanged']
    (output / 'study.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'passed': result['passed'], 'output': str(output), 'cases': len(result.get('cases', [])),
                      'selectedSyntheticCandidate': result.get('selectedSyntheticCandidate'),
                      'candidateEvaluations': result.get('candidateEvaluations'),
                      'negativeControls': result.get('negativeControls'), 'error': result.get('error')}))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
