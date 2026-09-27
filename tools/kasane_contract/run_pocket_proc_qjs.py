"""Compile and run the real QuickJS procedural adapter on the host."""
import os
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[2]
QJS=ROOT/'components/quickjs-ng/quickjs-ng'
OUT=ROOT/'.cache/kasane_pocket_proc_qjs'

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out',type=Path,help='write 48 RGB565 frames and SHA256 manifest')
    args=parser.parse_args()
    cc=shutil.which('cc') or shutil.which('gcc') or shutil.which('clang')
    if not cc:
        candidate=Path('C:/msys64/ucrt64/bin/gcc.exe')
        if candidate.exists():cc=str(candidate)
    if not cc:raise SystemExit('No host C compiler found')
    OUT.mkdir(parents=True,exist_ok=True)
    suffix='.exe' if sys.platform=='win32' else ''
    binary=OUT/('test_pocket_proc_qjs'+suffix)
    sources=[*(QJS/name for name in ('dtoa.c','libregexp.c','libunicode.c','quickjs.c','quickjs-vm.c')),
             ROOT/'main/ui/kasane/ksn_procedural.c',
             ROOT/'main/ui/kasane/ksn_proc_analysis.c',
             ROOT/'main/ui/kasane/ksn_proc_plan.c',
             ROOT/'main/ui/kasane/ksn_proc_plan_points.c',
             ROOT/'main/ui/kasane/ksn_proc_points.c',
             ROOT/'main/ui/kasane/ksn_proc_points_pie.c',
             ROOT/'main/ui/kasane/ksn_proc_points_dispatch.c',
             ROOT/'main/pocket/pocket_proc.c',
             ROOT/'tools/kasane_contract/test_pocket_proc_qjs.c']
    command=[cc,'-std=gnu11','-O2','-DQUICKJS_NG_BUILD','-D_GNU_SOURCE',
             '-DKSN_PROC_HOST_TEST',
             '-I',str(QJS),'-I',str(ROOT/'main'),'-I',str(ROOT/'main/pocket'),
             '-I',str(ROOT/'main/ui/kasane'),'-I',str(ROOT/'tools/kasane_contract'),
             '-I',str(ROOT/'tools/hostshim'),
             *(str(s) for s in sources),'-lm','-o',str(binary)]
    env=os.environ.copy();env['PATH']=str(Path(cc).parent)+os.pathsep+env.get('PATH','')
    subprocess.run(command,cwd=ROOT,env=env,check=True)
    invocation=[str(binary),str(ROOT/'apps/kasane/proc_megademo.js')]
    if args.out:
        args.out.mkdir(parents=True,exist_ok=True)
        invocation.append(str(args.out.resolve()))
    subprocess.run(invocation,cwd=ROOT,env=env,check=True)
    fake_binary=OUT/('test_pocket_proc_qjs_fake_pie'+suffix)
    fake_command=[argument for argument in command if argument!=str(ROOT/'main/ui/kasane/ksn_proc_points_pie.c')]
    fake_command[fake_command.index('-o')+1]=str(fake_binary)
    fake_command[1:1]=['-D__XTENSA__','-DCONFIG_IDF_TARGET_ESP32S3=1',
                       '-DKSN_PROC_HOST_FAKE_PIE']
    subprocess.run(fake_command,cwd=ROOT,env=env,check=True)
    subprocess.run([str(fake_binary),str(ROOT/'apps/kasane/proc_megademo.js')],
                   cwd=ROOT,env=env,check=True)
    if args.out:
        hashes={f'frame-{i:02d}.rgb565':hashlib.sha256(
            (args.out/f'frame-{i:02d}.rgb565').read_bytes()).hexdigest()
            for i in range(48)}
        (args.out/'sha256.json').write_text(json.dumps(hashes,indent=2)+'\n')
        print(f'Wrote {args.out}/sha256.json')

if __name__=='__main__':main()
