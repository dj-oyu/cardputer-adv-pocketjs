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
import tempfile

ROOT = Path(__file__).resolve().parents[3]
BASE = '7d21b3020b649aadc343c367f60948eca4abaf62'
sys.path.insert(0, str(ROOT / 'tools/kasane_contract'))
from run_megademo_app_host import build


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


BUILD_SCHEMA = 2
GENERATED = ('fonts.h', 'kasane_pet_test_data.h', 'ksn_proc_rom_plans.c', 'model_study_blob.h')
OBJECTS = ('dtoa.o', 'libregexp.o', 'libunicode.o', 'quickjs.o', 'quickjs-vm.o')


def compile_sources(root):
    """Conservative repository input closure, including generators' real inputs."""
    files = set()
    for name in ('main', 'tools', 'components/quickjs-ng/quickjs-ng',
                 'components/pocketjs_guest', 'apps/kasane'):
        for file in (root / name).rglob('*'):
            if not file.is_file() or any(p in ('.git', '.cache', '__pycache__', 'node_modules', 'build') for p in file.relative_to(root).parts):
                continue
            if file.suffix in ('.o', '.a', '.so', '.su', '.pyc', '.exe'):
                continue
            files.add(file)
    files.add(root / 'apps/pet/assets/pets-compact.bin')
    files.add(root / 'apps/cloud/assets/cloud.aynm')
    files.add(root / 'apps/cloud/assets/cloud_model_bytes.inc')
    fox_assets=root/'apps/fox3d/assets'
    if fox_assets.exists():
        files.update(p for p in fox_assets.rglob('*') if p.is_file() and p.suffix in ('.aynm','.inc'))
    return {file.relative_to(root).as_posix(): sha(file) for file in sorted(files)}


def content_key(sources, flags, blob_hash, toolchain):
    payload = dict(schema=BUILD_SCHEMA, sources=sources, flags=flags,
                   blob_sha256=blob_hash, toolchain=toolchain)
    return hashlib.sha256(json.dumps(payload, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def artifact_hashes(cache):
    return {name: sha(cache / name) for name in (*GENERATED, *OBJECTS)}


def object_cache_valid(cache, manifest, key):
    return bool(manifest and manifest.get('schema') == BUILD_SCHEMA and manifest.get('content_key') == key and
                all((cache / name).is_file() and sha(cache / name) == manifest.get('artifacts', {}).get(name) for name in OBJECTS))


def verify_reuse(manifest, binary, key):
    if manifest.get('schema') != BUILD_SCHEMA or manifest.get('content_key') != key:
        raise RuntimeError('reused binary has an obsolete schema or changed compile inputs/profile/blob')
    if manifest.get('binary_sha256') != sha(binary) or manifest.get('artifacts') != artifact_hashes(binary.parent):
        raise RuntimeError('reused binary, generated headers/ROM table, or cached objects changed')


def compiler_identity():
    result = {}
    for name in ('gcc', 'node'):
        executable = shutil.which(name)
        if not executable:
            raise RuntimeError(f'{name} is required')
        file = Path(executable).resolve()
        result[name] = dict(path=str(file), sha256=sha(file),
                            version=subprocess.check_output([name, '--version'], text=True).splitlines()[0])
    return result


def manifest_selftest():
    """One-factor stale-input tests in fresh temporary trees, never ROOT."""
    source_names = ('main/pocket/pocket_proc.c', 'main/ui/kasane/ksn_procedural.h',
                    'components/quickjs-ng/quickjs-ng/quickjs.c',
                    'components/quickjs-ng/quickjs-ng/quickjs.h', 'tools/build_kasane_test.sh',
                    'tools/make_font.py', 'apps/pet/assets/pets-compact.bin',
                    'apps/cloud/assets/cloud.aynm', 'apps/cloud/assets/cloud_model_bytes.inc',
                    'apps/fox3d/assets/fox_face.aynm', 'apps/fox3d/assets/fox_face_bytes.inc',
                    'tools/ayatsuri/model/export_fox_face.cjs', 'tools/ayatsuri/model/fixtures/fox_face_25.js',
                    'apps/kasane/megademo_plans.js', 'tools/kasane_ir/emit_rom_plans.mjs')
    cases = [('source', name) for name in source_names] + [('artifact', name) for name in (*GENERATED, *OBJECTS)] + [
        ('binary', ''), ('flags', ''), ('blob', ''), ('toolchain', ''), ('schema', ''), ('added_header', '')]
    with tempfile.TemporaryDirectory(prefix='ayatsuri-manifest-') as tmp:
        for index, (kind, name) in enumerate(cases):
            root = Path(tmp) / str(index)
            cache = root / '.cache/build'
            cache.mkdir(parents=True)
            for source in source_names:
                file = root / source
                file.parent.mkdir(parents=True, exist_ok=True)
                file.write_bytes(source.encode())
            for artifact in (*GENERATED, *OBJECTS):
                (cache / artifact).write_bytes(artifact.encode())
            binary = cache / 'binary'
            binary.write_bytes(b'binary')
            flags, blob, toolchain = ['-m32'], 'blob', {'gcc': 'fixture'}
            key = content_key(compile_sources(root), flags, blob, toolchain)
            manifest = dict(schema=BUILD_SCHEMA, content_key=key, binary_sha256=sha(binary), artifacts=artifact_hashes(cache))
            verify_reuse(manifest, binary, key)
            assert object_cache_valid(cache, manifest, key)
            if kind == 'source':
                with (root / name).open('ab') as f:
                    f.write(b' changed')
            elif kind == 'artifact':
                (cache / name).write_bytes(b'changed')
            elif kind == 'binary':
                binary.write_bytes(b'changed')
            elif kind == 'flags':
                flags.append('-O2')
            elif kind == 'blob':
                blob = 'changed'
            elif kind == 'toolchain':
                toolchain = {'gcc': 'changed'}
            elif kind == 'schema':
                manifest['schema'] = 1
            else:
                (root / 'main/pocket/new.h').write_bytes(b'new header')
            new_key = content_key(compile_sources(root), flags, blob, toolchain)
            try:
                verify_reuse(manifest, binary, new_key)
            except RuntimeError:
                pass
            else:
                raise AssertionError(f'stale input accepted: {kind} {name}')
            if kind in ('source', 'flags', 'blob', 'toolchain', 'schema', 'added_header') or kind == 'artifact' and name in OBJECTS:
                assert not object_cache_valid(cache, manifest, new_key)
    print(f'MODEL_MANIFEST_SELFTEST_PASS unchanged_positive={len(cases)} stale_negative={len(cases)} shared_tree_writes=0')


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
 },trySecond(){this.secondName='';this.secondCode='';this.secondReason='';
  try{this.create(1);globalThis.__studySecondOk=true;}catch(error){
   globalThis.__studySecondOk=false;this.secondReason=error===null?'null':typeof error;
   try{if(error&&typeof error==='object'){
    this.secondName=String(error.name||'').slice(0,80);this.secondCode=String(error.code||'').slice(0,80);
    this.secondReason=(this.secondName+'/'+this.secondCode).slice(0,161);
   }}catch(ignored){this.secondReason='unreadable error metadata';}
   if(typeof AyatsuriNative!=='undefined')throw error;
  }
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
    p.add_argument('--self-test-manifest', action='store_true', help='pure temporary-tree positive/stale-input tests; no build/run')
    args = p.parse_args()
    if args.self_test_manifest:
        manifest_selftest()
        return
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
    if args.reuse_binary:
        cache = args.reuse_binary.resolve().parent
    cache.mkdir(parents=True, exist_ok=True)
    blob = args.model_bin.resolve()
    data = blob.read_bytes()
    header = cache / 'model_study_blob.h'
    header_bytes = ('static const uint8_t model_study_blob[]={' + ','.join(map(str, data)) + '};\n').encode()
    if not args.reuse_binary:
        header.write_bytes(header_bytes)
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
    build_hashes, toolchain, blob_hash = compile_sources(ROOT), compiler_identity(), sha(blob)
    key = content_key(build_hashes, flags, blob_hash, toolchain)
    manifest_path = cache / 'model-study-build.json'
    if not args.reuse_binary:
        previous = json.loads(manifest_path.read_text()) if manifest_path.exists() else None
        if not object_cache_valid(cache, previous, key):
            for obj in cache.glob('*.o'):
                obj.unlink()
        binary = build(flags, cache, 'tools/kasane_contract/test_character_app_host.c')
        if compile_sources(ROOT) != build_hashes or compiler_identity() != toolchain or sha(blob) != blob_hash:
            raise RuntimeError('compiled inputs changed during build; rerun after source freeze')
        if header.read_bytes() != header_bytes:
            raise RuntimeError('generated model header differs from the requested blob')
        manifest_path.write_text(json.dumps(dict(schema=BUILD_SCHEMA, content_key=key,
            binary_sha256=sha(binary), flags=flags, blob_sha256=blob_hash, source_hashes=build_hashes,
            toolchain=toolchain, artifacts=artifact_hashes(cache)), indent=2) + '\n')
    else:
        binary = args.reuse_binary.resolve()
        compiled = json.loads(manifest_path.read_text())
        verify_reuse(compiled, binary, key)
        if header.read_bytes() != header_bytes:
            raise RuntimeError('reused generated model header differs from the requested blob')
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
    for env_key in ('CHARACTER_CAPTURE_DIR', 'CHARACTER_CAPTURE_PLAN', 'CHARACTER_FIXTURE'):
        env.pop(env_key, None)
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
        build_manifest_schema=BUILD_SCHEMA, build_content_key=key, build_manifest_sha256=sha(manifest_path),
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
