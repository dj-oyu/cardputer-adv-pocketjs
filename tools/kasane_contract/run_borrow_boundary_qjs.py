"""Isolated borrowed-input safety prototype; not firmware/product integration."""
import os
from pathlib import Path
import shutil
import subprocess
ROOT=Path(__file__).resolve().parents[2]
cc=shutil.which('cc') or shutil.which('gcc') or shutil.which('clang')
if not cc and Path('C:/msys64/ucrt64/bin/gcc.exe').exists():cc='C:/msys64/ucrt64/bin/gcc.exe'
if not cc:raise SystemExit('No host C compiler')
env=os.environ.copy();env['PATH']=str(Path(cc).parent)+os.pathsep+env['PATH']
out=ROOT/'.cache/grid-borrow';out.mkdir(parents=True,exist_ok=True)
qjs=ROOT/'components/quickjs-ng/quickjs-ng'
binary=out/('test-borrow'+('.exe' if os.name=='nt' else ''))
sources=[qjs/n for n in ('dtoa.c','libregexp.c','libunicode.c','quickjs.c','quickjs-vm.c')]
sources.append(ROOT/'tools/kasane_contract/test_borrow_boundary_qjs.c')
subprocess.run([cc,'-std=gnu11','-O2','-D_GNU_SOURCE','-DQUICKJS_NG_BUILD','-I'+str(qjs),
    *map(str,sources),'-lm','-o',str(binary)],env=env,check=True)
subprocess.run([str(binary)],env=env,check=True)
