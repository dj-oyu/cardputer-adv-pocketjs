"""Host lifecycle regression; uses an existing host compiler on Windows/Linux."""
import os
from pathlib import Path
import shutil
import subprocess
ROOT=Path(__file__).resolve().parents[2]
cc=shutil.which('cc') or shutil.which('gcc') or shutil.which('clang')
if not cc and Path('C:/msys64/ucrt64/bin/gcc.exe').exists():
    cc='C:/msys64/ucrt64/bin/gcc.exe'
if not cc:raise SystemExit('No host C compiler')
out=ROOT/'.cache/image-release';out.mkdir(parents=True,exist_ok=True)
env=os.environ.copy();env['PATH']=str(Path(cc).parent)+os.pathsep+env['PATH']
for opt in ('-O0','-O2'):
    binary=out/('test-'+opt[1:]+('.exe' if os.name=='nt' else ''))
    command=[cc,'-std=c11','-Wall','-Wextra','-Werror',opt,'-I'+str(ROOT/'main/ui/kasane'),
        '-I'+str(ROOT/'tools/kasane_contract'),str(ROOT/'main/ui/kasane/ksn_core.c'),
        str(ROOT/'tools/kasane_contract/test_image_release.c'),'-o',str(binary)]
    subprocess.run(command,env=env,check=True)
    subprocess.run([str(binary)],env=env,check=True)
