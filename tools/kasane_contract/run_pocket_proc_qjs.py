"""Compile and run the real QuickJS procedural adapter on the host."""
import os
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

from megademo_chunks import chunk_arg, rom_table
ROOT=Path(__file__).resolve().parents[2]
QJS=ROOT/'components/quickjs-ng/quickjs-ng'
OUT=ROOT/'.cache/kasane_pocket_proc_qjs'

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out',type=Path,help='write 48 RGB565 frames and SHA256 manifest')
    parser.add_argument('--opt',choices=('0','2'),default='2',help='host compiler optimization level')
    parser.add_argument('--proc-source',type=Path,help='alternate pocket_proc.c for baseline comparison')
    parser.add_argument('--clip-only',action='store_true',help='current native clipped-patch contracts, without MEGADEMO/baseline workload')
    parser.add_argument('--m32',action='store_true',help='clip-only i386/SSE2 profile, without sanitizers')
    args=parser.parse_args()
    if args.clip_only and (args.proc_source or args.out):parser.error('--clip-only cannot use --proc-source/--out')
    if args.m32 and (not args.clip_only or sys.platform=='win32'):parser.error('--m32 requires --clip-only under Linux/WSL')
    cc=shutil.which('cc') or shutil.which('gcc') or shutil.which('clang')
    if not cc:
        candidate=Path('C:/msys64/ucrt64/bin/gcc.exe')
        if candidate.exists():cc=str(candidate)
    if not cc:raise SystemExit('No host C compiler found')
    OUT.mkdir(parents=True,exist_ok=True)
    suffix='.exe' if sys.platform=='win32' else ''
    label='_clip' if args.clip_only else '_baseline' if args.proc_source else ''
    if args.m32:label+='_m32'
    binary=OUT/('test_pocket_proc_qjs'+label+'_O'+args.opt+suffix)
    sources=[*(QJS/name for name in ('dtoa.c','libregexp.c','libunicode.c','quickjs.c','quickjs-vm.c')),
             ROOT/'main/ui/kasane/ksn_procedural.c',
             ROOT/'main/ui/kasane/ksn_proc_analysis.c',
             ROOT/'main/ui/kasane/ksn_proc_plan.c',
             ROOT/'main/ui/kasane/ksn_proc_plan_points.c',
             ROOT/'main/ui/kasane/ksn_proc_points.c',
             ROOT/'main/ui/kasane/ksn_proc_points_pie.c',
             ROOT/'main/ui/kasane/ksn_proc_points_dispatch.c',
             ROOT/'main/ui/kasane/ksn_core.c',
             ROOT/'main/ui/kasane/ksn_render.c',
             ROOT/'main/ui/kasane/ksn_blend_pie.c',
             (args.proc_source.resolve() if args.proc_source else ROOT/'main/pocket/pocket_proc.c'),
             ROOT/'main/pocket/pocket_shape.c', ROOT/'main/ayatsuri/ayatsuri_model.c', ROOT/'main/pocket/pocket_ayatsuri_assets.c',
             ROOT/'tools/kasane_contract/proc_native_weak.c',
             ROOT/('tools/kasane_contract/test_pocket_proc_clip_qjs.c' if args.clip_only else 'tools/kasane_contract/test_pocket_proc_qjs.c'),
             # MEGADEMO's built-in plans (the app registers kasane.<name>).
             rom_table(OUT/'ksn_proc_rom_plans.c')]
    flags=['-ffp-contract=off'] if args.clip_only else []
    env=os.environ.copy();env['PATH']=str(Path(cc).parent)+os.pathsep+env.get('PATH','')
    if args.m32:
        env.setdefault('M32_SYSROOT','/tmp/m32sys')
        # Build and execute in this same process: the temporary sysroot may
        # disappear between WSL invocations. No parallel helper is started.
        flags+=subprocess.check_output(['bash','tools/vmtest/m32_sysroot.sh'],cwd=ROOT,env=env,text=True).split()
        flags+=['-msse2','-mfpmath=sse']
    elif args.clip_only and sys.platform!='win32':flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
    command=[cc,'-std=gnu11','-O'+args.opt,*flags,'-DQUICKJS_NG_BUILD','-D_GNU_SOURCE',
             '-DKSN_PROC_HOST_TEST',
             '-I',str(QJS),'-I',str(ROOT/'main'),'-I',str(ROOT/'main/pocket'),
             '-I',str(ROOT/'main/ui/kasane'),'-I',str(ROOT/'tools/kasane_contract'),
             '-I',str(ROOT/'tools/hostshim'),
             *(str(s) for s in sources),'-lm','-o',str(binary)]
    subprocess.run(command,cwd=ROOT,env=env,check=True)
    if args.clip_only:
        env.setdefault('ASAN_OPTIONS','detect_leaks=0:abort_on_error=1')
        env.setdefault('UBSAN_OPTIONS','halt_on_error=1:print_stacktrace=1')
        subprocess.run([str(binary)],cwd=ROOT,env=env,check=True)
        return
    # The app's chunks, then Act I's former programs (the VM reference).
    files=chunk_arg()+','+str(ROOT/'tools/kasane_contract/proc_megademo_news_ref.js')
    invocation=[str(binary),files]
    if args.out:
        args.out.mkdir(parents=True,exist_ok=True)
        invocation.append(str(args.out.resolve()))
    subprocess.run(invocation,cwd=ROOT,env=env,check=True)
    fake_binary=OUT/('test_pocket_proc_qjs_fake_pie'+label+'_O'+args.opt+suffix)
    fake_command=[argument for argument in command if argument!=str(ROOT/'main/ui/kasane/ksn_proc_points_pie.c')]
    fake_command[fake_command.index('-o')+1]=str(fake_binary)
    fake_command[1:1]=['-D__XTENSA__','-DCONFIG_IDF_TARGET_ESP32S3=1',
                       '-DKSN_PROC_HOST_FAKE_PIE']
    subprocess.run(fake_command,cwd=ROOT,env=env,check=True)
    subprocess.run([str(fake_binary),files],
                   cwd=ROOT,env=env,check=True)
    if args.out:
        hashes={f'frame-{i:02d}.rgb565':hashlib.sha256(
            (args.out/f'frame-{i:02d}.rgb565').read_bytes()).hexdigest()
            for i in range(48)}
        (args.out/'sha256.json').write_text(json.dumps(hashes,indent=2)+'\n')
        print(f'Wrote {args.out}/sha256.json')

if __name__=='__main__':main()
