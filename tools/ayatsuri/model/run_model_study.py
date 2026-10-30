"""Opt-in host A/C study. No device/ESP free-heap or performance claim."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
BASE = '7d21b3020b649aadc343c367f60948eca4abaf62'
sys.path.insert(0, str(ROOT / 'tools/kasane_contract'))
from run_megademo_app_host import build


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def git(*args):
    attempts = [['git', '-C', str(ROOT), *args]]
    windows_git = Path('/mnt/c/Program Files/Git/cmd/git.exe')
    if windows_git.exists():
        winroot = subprocess.check_output(['wslpath', '-w', str(ROOT)], text=True).strip()
        attempts.append([str(windows_git), '-C', winroot, *args])
    for command in attempts:
        result = subprocess.run(command, capture_output=True)
        if result.returncode == 0:
            return result.stdout
    raise RuntimeError(result.stderr.decode(errors='replace'))


def baseline(folder):
    """Fill missing JS dependencies from the exact revision, never overwrite."""
    manifest = folder / 'js-dependencies.json'
    if manifest.exists():
        saved = json.loads(manifest.read_text())
        if saved['head'] != BASE or any(sha(folder / name) != value for name, value in saved['sha256'].items()):
            raise RuntimeError('frozen baseline dependency manifest changed')
        return
    names = git('ls-tree', '-r', '--name-only', BASE, 'apps/cloud', 'apps/cloudprobe',
                'apps/character', 'apps/lib').decode().splitlines()
    hashes = {}
    for name in names:
        if not (name.endswith('.js') or name.endswith('chunks.txt')):
            continue
        data = git('show', f'{BASE}:{name}')
        target = folder / name
        if target.exists():
            if target.read_bytes() != data:
                raise RuntimeError(f'refuse to overwrite changed baseline: {target}')
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        hashes[name] = sha(target)
    manifest.write_text(json.dumps(dict(head=BASE, sha256=hashes), indent=2) + '\n')


def driver():
    # Both routes use the same model sampling/update code and explicit camera.
    # Ordinary app private actor remains unconstructed; this is a no-UI study.
    return """'use strict';
globalThis.__modelStudy=(()=>{
 const a=[null,null],H=pocket.kasane.procedural,resource=H.resource({maxSegments:1024}),
 light={direction:[-.5,-.4,.8],color:0xffffff},
 camera={position:[0,0,400],target:[0,0,0],zoom:Math.fround(.85),center:[120,68],projection:'orthographic'};
 pocket.kasane.replace(tx=>{tx.background(0xe4ece0ff);tx.image({resource,bounds:[0,0,240,135],sourceWidth:240,sourceHeight:135})});
 return {create(i){let scene;
  try{scene=typeof AyatsuriNative==='undefined'?Ayatsuri.scene({backend:pocket.hone,geometries:[AyatsuriBlend],light,camera}):AyatsuriNative.scene({model:AyatsuriNativeCloudModel(),light,camera});
   const model=AyatsuriModels.create(scene,0);if(model.sampleFactory)model.sample=model.sampleFactory(model.sample);
   a[i]={scene,model,input:{body:{},expression:{}}};
  }catch(error){if(scene)scene.dispose();throw error;}
 },trySecond(){try{this.create(1);globalThis.__studySecondOk=true;}catch(error){globalThis.__studySecondOk=false;}
 },frame(i,tick){const x=a[i],t=tick*1000/30;
  x.model.sample(t,0,false,x.input);x.model.update(t,x.input.body,x.input.expression);x.scene.evaluate(t);
  H.beginFrame(0xe77c);x.scene.render();H.commit();
 },dispose(i){if(a[i])a[i].scene.dispose();a[i]=null;}};
})();
"""


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--variant', choices=('A', 'C'), default='A')
    p.add_argument('--output', type=Path, default=ROOT / '.cache/native-model-study/run')
    p.add_argument('--baseline-root', type=Path, default=ROOT / '.cache/native-model-study/baseline-7d21b30')
    p.add_argument('--model-bin', type=Path, default=ROOT / '.cache/ayatsuri-native/cloud.aynm')
    p.add_argument('--reuse-binary', type=Path)
    p.add_argument('--frames', type=int, default=301)
    p.add_argument('--heap-limit', type=int, default=163840)
    p.add_argument('--m32', action='store_true')
    p.add_argument('--gc-probe', action='store_true', help='secondary post-dispose explicit GC only')
    args = p.parse_args()
    if not 1 <= args.frames <= 3600:
        p.error('frames must be in 1..3600')
    folder = args.output.resolve()
    folder.mkdir(parents=True, exist_ok=True)
    base = args.baseline_root.resolve()
    baseline(base)
    app_root = base if args.variant == 'A' else ROOT
    entry = app_root / ('apps/cloudprobe/cloud_probe.js' if args.variant == 'A' else 'apps/cloudprobe/native_probe.js')
    chunks = app_root / ('apps/cloudprobe/chunks.txt' if args.variant == 'A' else 'apps/cloudprobe/native_chunks.txt')
    cache = folder / ('build-m32' if args.m32 else 'build-asan')
    cache.mkdir(parents=True, exist_ok=True)
    blob = args.model_bin.resolve()
    data = blob.read_bytes()
    header = cache / 'model_study_blob.h'
    header.write_text('static const uint8_t model_study_blob[]={' + ','.join(map(str, data)) + '};\n')
    flags = ['-DCHARACTER_MODEL_STUDY=1', '-DCHARACTER_MODEL_BLOB=1', '-fstack-usage', '-ffp-contract=off',
             '-Wl,--wrap=malloc', '-Wl,--wrap=realloc']
    if args.m32:
        env = dict(os.environ, M32_SYSROOT=os.environ.get('M32_SYSROOT', '/tmp/m32sys'))
        flags = subprocess.check_output(['bash', 'tools/vmtest/m32_sysroot.sh'], cwd=ROOT, env=env, text=True).split() + flags
        flags += ['-msse2', '-mfpmath=sse']
    else:
        flags += ['-fsanitize=address,undefined']
    compat = ROOT / '.cache/ci-compat/bin'
    if compat.is_dir():
        os.environ['PATH'] = str(compat) + os.pathsep + os.environ.get('PATH', '')
    stamp = cache / 'model-study-flags.json'
    build_inputs = set(ROOT.glob('main/ayatsuri/*')) | set(ROOT.glob('main/pocket/*.inc'))
    build_inputs.update(ROOT / name for name in ('main/pocket/pocket_shape.c',
        'tools/kasane_contract/test_character_app_host.c', 'tools/kasane_contract/test_megademo_app_host.c',
        'tools/kasane_contract/run_megademo_app_host.py'))
    build_hashes = {str(x): sha(x) for x in sorted(build_inputs) if x.is_file()}
    if not args.reuse_binary:
        if not stamp.exists() or json.loads(stamp.read_text()) != flags:
            for obj in cache.glob('*.o'):
                obj.unlink()
        stamp.write_text(json.dumps(flags))
        binary = build(flags, cache, 'tools/kasane_contract/test_character_app_host.c')
        if any(sha(Path(name)) != value for name, value in build_hashes.items()):
            raise RuntimeError('compiled inputs changed during build; rerun after source freeze')
        (binary.parent / 'model-study-build.json').write_text(json.dumps(dict(
            binary_sha256=sha(binary), flags=flags, blob_sha256=sha(blob), source_hashes=build_hashes), indent=2))
    else:
        binary = args.reuse_binary.resolve()
        compiled = json.loads((binary.parent / 'model-study-build.json').read_text())
        if compiled['binary_sha256'] != sha(binary) or compiled['blob_sha256'] != sha(blob) or compiled['flags'] != flags or compiled['source_hashes'] != build_hashes:
            raise RuntimeError('reused binary/build profile/blob/source hashes do not match its build manifest')
    script = folder / 'study_driver.js'
    script.write_text(driver())
    inputs = {entry, chunks, script, blob, Path(__file__)}
    for line in chunks.read_text().splitlines():
        words = line.split('#', 1)[0].split()
        if len(words) == 2 and words[0] not in ('app', 'option'):
            inputs.add((chunks.parent / words[1]).resolve())
    for sub in ('apps/cloud', 'apps/cloudprobe'):
        inputs.update(path for path in (ROOT / sub).rglob('*') if path.is_file())
    hashes = {str(path): sha(path) for path in sorted(inputs)}
    env = dict(os.environ, CHARACTER_APP_ENTRY=str(entry), CHARACTER_APP_CHUNKS=str(chunks),
               CHARACTER_MEMORY_STUDY=str(folder / (args.variant + '.jsonl')),
               CHARACTER_MODEL_DRIVER=str(script), CHARACTER_MODEL_FRAMES=str(args.frames),
               CHARACTER_MODEL_PIXELS=str(folder / (args.variant + '-pixels.csv')),
               CHARACTER_HEAP_LIMIT=str(args.heap_limit), CHARACTER_NO_EAGER_GC='1', CHARACTER_FULL_REFERENCE='1')
    for key in ('CHARACTER_CAPTURE_DIR', 'CHARACTER_CAPTURE_PLAN', 'CHARACTER_FIXTURE'):
        env.pop(key, None)
    if args.gc_probe:
        env['CHARACTER_MODEL_GC_PROBE'] = '1'
    else:
        env.pop('CHARACTER_MODEL_GC_PROBE', None)
    env.setdefault('ASAN_OPTIONS', 'detect_leaks=0:abort_on_error=1')
    env.setdefault('UBSAN_OPTIONS', 'halt_on_error=1:print_stacktrace=1')
    for path in (Path(env['CHARACTER_MEMORY_STUDY']), Path(env['CHARACTER_MODEL_PIXELS'])):
        path.unlink(missing_ok=True)
    run = subprocess.run([str(binary)], cwd=ROOT, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (folder / (args.variant + '.log')).write_text(run.stdout)
    print(run.stdout, end='')
    observations = Path(env['CHARACTER_MEMORY_STUDY'])
    rows = [json.loads(line) for line in observations.read_text().splitlines()] if observations.exists() else []
    current_hashes = {name: sha(Path(name)) for name in hashes}
    result = dict(kind='HOST SIMULATION', variant=args.variant, baseline_head=BASE,
        build_head=git('rev-parse', 'HEAD').decode().strip(), passed=run.returncode == 0 and 'MODEL_STUDY_PASS' in run.stdout,
        returncode=run.returncode, source_hashes=hashes, sources_unchanged_during_run=hashes == current_hashes,
        binary=str(binary), binary_sha256=sha(binary), flags=flags, model_rom_const_bytes=len(data),
        explicit_gc_primary=False, explicit_gc_secondary=args.gc_probe, rows=rows,
        B='UNMEASURED: rejected old importer not ported to current runtime',
        scope='link-wrapped explicit requested native bytes include fixture; libc-internal allocations outside linked calls are unmeasured',
        native_free_bytes=None, native_largest_free_bytes=None, device_measurement=False,
        packet_copy_bytes='UNMEASURED until packet wrapper enabled',
        camera_profile='shared explicit Float32 zoom; no general Number-camera equivalence claim',
        stack_usage_files=[str(x) for x in cache.glob('*.su')],
        xtensa_compiler=shutil.which('xtensa-esp32s3-elf-gcc'))
    (folder / (args.variant + '-result.json')).write_text(json.dumps(result, indent=2) + '\n')
    print('MODEL_STUDY_RESULT', folder / (args.variant + '-result.json'))
    if not result['passed'] or not result['sources_unchanged_during_run']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
