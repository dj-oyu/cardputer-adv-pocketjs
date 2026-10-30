"""Real FOX FACE app and native renderer; host simulation, never device FPS."""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import subprocess
import sys

from run_megademo_app_host import ROOT, build
sys.path.insert(0,str(ROOT/'tools/ayatsuri/model'))
from run_model_study import compile_sources, compiler_identity, content_key


def inputs():
    found=compile_sources(ROOT)
    folder=ROOT/'apps/fox3d'
    paths={folder/'fox_face.js',folder/'chunks.txt',Path(__file__).resolve(),
           ROOT/'tools/kasane_contract/test_fox_app_host.c'}
    for line in (folder/'chunks.txt').read_text().splitlines():
        words=line.split('#',1)[0].split()
        if len(words)==2 and words[0] not in ('app','option'):
            path=(folder/words[1]).resolve();path.relative_to(ROOT.resolve());paths.add(path)
    found.update({str(p.resolve().relative_to(ROOT.resolve())).replace('\\','/'):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths})
    return found


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--m32',action='store_true')
    parser.add_argument('--capture-dir',type=Path)
    args=parser.parse_args()
    cache=ROOT/('.cache/fox-app-m32' if args.m32 else '.cache/fox-app-asan');cache.mkdir(parents=True,exist_ok=True)
    (cache/'result.json').unlink(missing_ok=True)
    flags=['-Itools/kasane_contract','-ffp-contract=off']
    env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    if args.m32:
        env.setdefault('M32_SYSROOT','/tmp/m32sys')
        flags+=subprocess.check_output(['bash','tools/vmtest/m32_sysroot.sh'],cwd=ROOT,env=env,text=True).split()+['-msse2','-mfpmath=sse']
    else:flags+=['-fsanitize=address,undefined']
    before=inputs();toolchain=compiler_identity()
    # build() always recompiles/links the fixture and native C. Only its QuickJS
    # objects are cached: their source/header, compiler and flag closure is fixed.
    qjs={k:v for k,v in before.items() if k.startswith(('components/quickjs-ng/','components/pocketjs_guest/include/'))
         or k=='tools/kasane_contract/run_megademo_app_host.py'}
    key=content_key(qjs,flags,'fox-qjs-object-cache-v1',toolchain)
    binary=build(flags,cache/key[:16],'tools/kasane_contract/test_fox_app_host.c')
    runs=[]
    for index in range(2):
        folder=None
        if args.capture_dir:
            folder=args.capture_dir/f'run-{index}';folder.mkdir(parents=True,exist_ok=True)
            for file in folder.glob('app_*.ppm'):file.unlink()
            env['FOX_CAPTURE_DIR']=str(folder.resolve())
        result=subprocess.run([str(binary)],cwd=ROOT,env=env,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
        (cache/f'run-{index}.log').write_text(result.stdout);print(result.stdout,end='')
        match=re.search(r'FOX_HOST frames=(\d+) pointer_bits=(\d+) peak_requested=(\d+) max_request=(\d+) cap=(\d+) eager_gc=0 full_reference=1 quota=(\d+) allocator=(\d+)',result.stdout)
        metric_names=('frames','pointer_bits','peak_requested','max_request','cap','quota','allocator')
        metrics=dict(zip(metric_names,map(int,match.groups()))) if match else None
        profile_valid=bool(metrics and metrics['frames']==231 and metrics['pointer_bits']==(32 if args.m32 else 64)
                           and metrics['cap']==(163840 if args.m32 else 0) and not metrics['quota'] and not metrics['allocator'])
        complete=True;images=[]
        if folder:
            header=b'P6\n240 135\n255\n'
            for i in range(12):
                path=folder/f'app_{i:04}.ppm';data=path.read_bytes() if path.exists() else b''
                complete=complete and data.startswith(header) and len(data)==len(header)+240*135*3
                images.append(hashlib.sha256(data).hexdigest())
        runs.append(dict(exit_code=result.returncode,captures_complete=complete,image_sha256=images,metrics=metrics,
                         passed=result.returncode==0 and 'FOX_HOST_PASS' in result.stdout and complete and profile_valid))
    unchanged=inputs()==before
    same_images=runs[0]['image_sha256']==runs[1]['image_sha256'] if args.capture_dir else None
    proof=dict(kind='HOST SIMULATION',device_capture=False,passed=all(r['passed'] for r in runs) and unchanged and same_images is not False,
               cap=163840 if args.m32 else None,forced_frame_gc=False,pointer_profile=32 if args.m32 else 64,runs=runs,
               binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),flags=flags,toolchain=toolchain,
               source_sha256=before,sources_unchanged_during_run=unchanged,repeat_images_equal=same_images,
               repeat_image_comparison_performed=bool(args.capture_dir))
    (cache/'result.json').write_text(json.dumps(proof,indent=2)+'\n')
    if args.capture_dir and all(r['captures_complete'] for r in runs):
        sys.path.insert(0,str(ROOT/'tools/character'))
        from fox_contact import write_contact
        for index in range(2):print(write_contact(args.capture_dir/f'run-{index}'))
    if not proof['passed']:raise SystemExit(1)


if __name__=='__main__':main()
